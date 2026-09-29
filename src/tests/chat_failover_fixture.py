# This file is part of Project SkyFire https://www.projectskyfire.org.
# See LICENSE.md file for Copyright information
"""Two real native daemons sharing the character service's durable realm lease."""
import asyncio
import json
import socket
import struct
import subprocess
import sys
from pathlib import Path
from chat_transport_fixture import _refresh, service, string


async def exercise(fixture):
    with socket.socket() as reserve:
        reserve.bind(('127.0.0.1', 0))
        standby_port = reserve.getsockname()[1]
    primary_port = fixture.port
    config = (fixture.root/'chat.conf').read_text().replace(
        f'Chat.Port = {primary_port}', f'Chat.Port = {standby_port}').replace(
        'Cluster.NodeKey = "chat-test"', 'Cluster.NodeKey = "chat-standby"').replace(
        '/chat-test.pem', '/chat-standby.pem').replace('/chat-test.key', '/chat-standby.key')
    (fixture.root/'standby.conf').write_text(config)
    fixture.standby_log = (fixture.root/'standby.log').open('wb')
    fixture.standby_process = await asyncio.create_subprocess_exec(str(Path(fixture.executable).resolve()),
        '-c', str(fixture.root/'standby.conf'), cwd=fixture.root, stdout=fixture.standby_log,
        stderr=fixture.standby_log, creationflags=subprocess.CREATE_NO_WINDOW if sys.platform=='win32' else 0)
    for _ in range(100):
        if fixture.standby_process.returncode is not None:
            fixture.standby_log.flush()
            fixture.fail('Standby exited before listening: '+(fixture.root/'standby.log').read_text(errors='replace')[-4000:])
        try:
            await fixture.exchange('world-a', 1, 1, accepted=False, port=standby_port)
            break
        except ConnectionError:
            await asyncio.sleep(.1)
    else:
        fixture.fail('Standby listener did not start')
    # A live owner prevents a second daemon from serving private traffic.
    await asyncio.sleep(1)
    await fixture.exchange('world-a', 1, 1, accepted=False, port=standby_port)
    await _refresh(fixture)
    driver = Path(fixture.executable).resolve().with_name('chat_failover_client.exe' if sys.platform=='win32' else 'chat_failover_client')
    fixture.assertTrue(driver.is_file(), 'Build the chat_failover_client integration target')
    client_config = '\n'.join(['[worldserver]', 'RealmID = 1', 'ChatService.Enable = 1', 'ChatService.Messages = 1',
        'ChatService.Host = "127.0.0.1"', f'ChatService.Port = {primary_port}', 'ChatService.NodeKey = "chat-test"',
        'ChatService.Standby.Host = "127.0.0.1"', f'ChatService.Standby.Port = {standby_port}',
        'ChatService.Standby.NodeKey = "chat-standby"'] +
        [f'Cluster.{key} = "{(fixture.root/name).as_posix()}"' for key,name in
         [('CA','ca.pem'),('Certificate','world-client.pem'),('PrivateKey','world-client.key')]])
    (fixture.root/'client.conf').write_text(client_config)
    fixture.client_process = await asyncio.create_subprocess_exec(str(driver),str(fixture.root/'client.conf'),
        cwd=fixture.root,stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.STDOUT,
        creationflags=subprocess.CREATE_NO_WINDOW if sys.platform=='win32' else 0)
    async def wait_client(endpoint):
        async def read():
            while True:
                line = await fixture.client_process.stdout.readline()
                if not line: fixture.fail('Native chat client stopped before endpoint selection')
                if line.strip() == ('CHAT_ENDPOINT '+endpoint).encode(): return
        await asyncio.wait_for(read(),30)
    await wait_client('chat-test')

    async def rpc(sequence, action, revision=0, value='', target=0):
        command = struct.pack('!BIQBQQII',1,77,revision,action,target,0,0,0)+string(value)
        response = await fixture.exchange('world-a',1,5,service(2,'a'*64,sequence,10,100,command))
        number,status,size = struct.unpack('!QBI',response[:13])
        fixture.assertEqual((number,size),(sequence,len(response)-13))
        return status,response[13:]
    status,roster = await rpc(20000,18)
    fixture.assertEqual(status,0)
    before = struct.unpack('!Q',roster[2:10])[0]
    fixture.lost_reply = dict(armed=True,attempts=0,dropped=False,reconciled=0,marker='Lost takeover fixture')
    try:
        status,_ = await rpc(20001,0,before,'Lost takeover fixture')
        fixture.assertIn(status,(0,2,3))
    except (asyncio.TimeoutError,asyncio.IncompleteReadError,ConnectionError):
        pass
    fixture.assertTrue(fixture.lost_reply['dropped'])
    fixture.process.kill()
    await fixture.process.wait()
    fixture.port = standby_port
    for _ in range(150):
        try:
            await fixture.exchange('world-a',1,1)
            break
        except (asyncio.TimeoutError,asyncio.IncompleteReadError,ConnectionError):
            await asyncio.sleep(.2)
    else:
        fixture.fail('Standby did not acquire the expired realm lease')
    # Old world presence is deliberately absent until a fresh full publication.
    command = struct.pack('!BIQBQQII',1,77,0,18,0,0,0,0)+string('')
    await fixture.exchange('world-a',1,5,service(2,'a'*64,20002,10,100,command),accepted=False)
    await _refresh(fixture)
    await wait_client('chat-standby')
    # Durable reconciliation is bound to original identity, even after logout.
    fixture._transport_presence_sequence += 1
    empty_presence = string('a'*64)+struct.pack('!QH',fixture._transport_presence_sequence,0)
    await fixture.exchange('world-a',1,2,empty_presence)
    status,recovered = await rpc(20003,21,target=20001)
    fixture.assertEqual(status,0,'New daemon must recover the original committed command')
    fixture.assertEqual(struct.unpack('!Q',recovered[2:10])[0],before+1)
    fixture.assertEqual(fixture.lost_reply['attempts'],1,'Takeover must never replay the mutation')
    await _refresh(fixture)
    with fixture.database_fixture.admin.cursor() as cursor:
        cursor.execute("SELECT revision,document FROM character_social_records WHERE realm=1 AND domain='guilds' AND record_key='guild-77'")
        revision,document = cursor.fetchone()
        fixture.assertEqual(revision,before+1)
        fixture.assertEqual(json.loads(document)['motd'],'Lost takeover fixture')
        cursor.execute('SELECT COUNT(*) FROM character_social_receipts WHERE request_id=%s',(fixture.lost_reply['receipt'],))
        fixture.assertEqual(cursor.fetchone()[0],1)
        cursor.execute('SELECT DISTINCT node FROM character_social_owners WHERE realm=1')
        fixture.assertEqual(cursor.fetchall(),(('chat-standby',),))
    # Resurrecting the old primary must leave it fenced while standby is healthy.
    fixture.process = await asyncio.create_subprocess_exec(str(Path(fixture.executable).resolve()),
        '-c',str(fixture.root/'chat.conf'),cwd=fixture.root,stdout=fixture.log,stderr=fixture.log,
        creationflags=subprocess.CREATE_NO_WINDOW if sys.platform=='win32' else 0)
    for _ in range(100):
        try:
            await fixture.exchange('world-a',1,1,accepted=False,port=primary_port)
            break
        except ConnectionError:
            await asyncio.sleep(.1)
    else:
        fixture.fail('Restarted primary did not start its listener')
    await asyncio.sleep(6)
    await fixture.exchange('world-a',1,1,accepted=False,port=primary_port)
    await fixture.exchange('world-a',1,1)
