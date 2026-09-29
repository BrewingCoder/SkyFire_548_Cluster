# This file is part of Project SkyFire https://www.projectskyfire.org.
# See LICENSE.md file for Copyright information
"""Exercise the real native chat listener with disposable mTLS identities and realms."""
import argparse
import asyncio
import contextlib
import datetime
import ipaddress
import json
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import unittest


def text(value):
    encoded = value.encode() if isinstance(value, str) else value
    return struct.pack('!H', len(encoded)) + encoded


def presence(generation, sequence, players):
    return text(generation) + struct.pack('!QH', sequence, len(players)) + b''.join(
        struct.pack('!IQQ', account, guid, incarnation) + text(name)
        for account, guid, incarnation, name in players)


def route(generation, sequence, kind, members, message='private', sender=100, incarnation=1, account=10):
    return (text(generation) + text('fixture-audience') + struct.pack('!BQH', kind, sequence, len(members)) +
            b''.join(struct.pack('!QQB', *member) for member in members) +
            struct.pack('!QQQII', sequence, sender, incarnation, account, 0) + text(message) + b'\0')


def service(domain, generation, sequence, account, actor, payload, incarnation=1):
    return bytes([domain])+text(generation)+struct.pack('!QIQQI',sequence,account,actor,incarnation,len(payload))+payload


def guild_command(action, revision=0, target=0, value=''):
    return struct.pack('!BIQBQQII',1,77,revision,action,target,0,0,0)+text(value)


def channel_command(action, password='', target=0, name='Fixture'):
    optional=lambda value: b'\1'+text(value) if value else b'\0'
    return bytes([action])+text(name)+struct.pack('!IIIIIQB',0,469,469,469,0,target,0)+optional(password)+b'\0\0'


class NativeChatTests(unittest.IsolatedAsyncioTestCase):
    executable = None
    database_config = None
    persistent = False

    async def asyncSetUp(self):
        if not self.executable:
            self.skipTest('Pass --chatserver with a built native executable')
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID
        self.temporary = tempfile.TemporaryDirectory(prefix='skyfire-chat-test-')
        self.addAsyncCleanup(self.cleanup)
        self.addAsyncCleanup(self.failure_logs)
        self.root = Path(self.temporary.name)
        self.process = self.hub = self.log = None
        self.standby_process = self.standby_log = None
        self.client_process = None
        self.character_task = self.character_stop = None
        self.hub_clients = set()
        self.hub_tasks = set()
        self.registered = asyncio.Event()
        now = datetime.datetime.now(datetime.timezone.utc)
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'Isolated chat fixture CA')])
        ca = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
              .serial_number(x509.random_serial_number()).not_valid_before(now-datetime.timedelta(minutes=1))
              .not_valid_after(now+datetime.timedelta(days=1))
              .add_extension(x509.BasicConstraints(ca=True, path_length=None), True).sign(key, hashes.SHA256()))
        (self.root/'ca.pem').write_bytes(ca.public_bytes(serialization.Encoding.PEM))
        for identity in ('hub', 'chat-test', 'chat-standby', 'world-a', 'world-b', 'world-client', 'outsider', 'characters-test'):
            private = rsa.generate_private_key(public_exponent=65537, key_size=2048)
            certificate = (x509.CertificateBuilder()
                           .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, identity)]))
                           .issuer_name(name).public_key(private.public_key()).serial_number(x509.random_serial_number())
                           .not_valid_before(now-datetime.timedelta(minutes=1)).not_valid_after(now+datetime.timedelta(days=1))
                           .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address('127.0.0.1'))]), False)
                           .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH, ExtendedKeyUsageOID.SERVER_AUTH]), False)
                           .sign(key, hashes.SHA256()))
            (self.root/(identity+'.pem')).write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
            (self.root/(identity+'.key')).write_bytes(private.private_bytes(serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        server_tls = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH, cafile=str(self.root/'ca.pem'))
        server_tls.load_cert_chain(self.root/'hub.pem', self.root/'hub.key')
        server_tls.verify_mode = ssl.CERT_REQUIRED
        self.hub = await asyncio.start_server(self.serve_hub, '127.0.0.1', 0, ssl=server_tls)
        hub_port = self.hub.sockets[0].getsockname()[1]
        persistence_config = ''
        if self.persistent:
            from characterserver import serve
            with socket.socket() as reserve:
                reserve.bind(('127.0.0.1', 0))
                character_port = reserve.getsockname()[1]
            service = Path(__file__).resolve().parents[1]/'server'/'characterserver'
            config = self.database_fixture.config | dict(node_key='characters-test',node_name='Fixture',
                bind_address='127.0.0.1',advertise_address='127.0.0.1',port=character_port,
                hub_host='127.0.0.1',hub_port=hub_port,ca=str(self.root/'ca.pem'),
                certificate=str(self.root/'characters-test.pem'),private_key=str(self.root/'characters-test.key'),
                catalog=str(service/'statements.json'),allowed_world_nodes=['world-a','world-b'],allowed_chat_nodes=['chat-test','chat-standby'])
            character_config = self.root/'character.toml'
            character_config.write_text('\n'.join(key+' = '+json.dumps(value) for key,value in config.items()))
            self.character_stop = asyncio.Event()
            self.character_task = asyncio.create_task(serve(character_config,self.character_stop,
                database_factory=self.character_database_factory))
            persistence_config = f'''\nChat.Persistence.Realm.1.Host = "127.0.0.1"
Chat.Persistence.Realm.1.Port = {character_port}
Chat.Persistence.Realm.1.NodeKey = "characters-test"
'''
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1', 0))
            self.port = reserve.getsockname()[1]
        def path(name):
            return (self.root/name).as_posix()
        config = f'''[chatserver]
ConfVersion = 2026092900
Chat.BindIP = "127.0.0.1"
Chat.Port = {self.port}
Chat.Realms = "1 2"
Chat.AllowedWorlds = "world-a world-b"
Chat.WorldRealms = "world-a=1 world-b=2"
Chat.RequestTimeout = 2
Chat.MaxConnections = 16
Chat.Persistence.Enable = 0
Cluster.Enable = 1
Cluster.NodeKey = "chat-test"
Cluster.NodeName = "Isolated fixture"
Cluster.HubHost = "127.0.0.1"
Cluster.HubPort = {hub_port}
Cluster.AdvertiseAddress = "127.0.0.1"
Cluster.Certificate = "{path('chat-test.pem')}"
Cluster.PrivateKey = "{path('chat-test.key')}"
Cluster.CA = "{path('ca.pem')}"
Cluster.Handoff.Enable = 0
Cluster.RealmDirectory.Enable = 0
Cluster.CRL = ""
LogsDir = "{self.root.as_posix()}"
Appender.Console = 1,3,0
Logger.root = 3,Console
'''
        if self.persistent:
            config = config.replace('Chat.Realms = "1 2"','Chat.Realms = "1"').replace(
                'world-b=2','world-b=1').replace('Chat.Persistence.Enable = 0','Chat.Persistence.Enable = 1')
            config += persistence_config
            config = config.replace('Chat.AllowedWorlds = "world-a world-b"', 'Chat.AllowedWorlds = "world-a world-b world-client"').replace(
                'Chat.WorldRealms = "world-a=1 world-b=1"', 'Chat.WorldRealms = "world-a=1 world-b=1 world-client=1"')
        (self.root/'chat.conf').write_text(config)
        self.log = (self.root/'native.log').open('wb')
        self.process = await asyncio.create_subprocess_exec(str(Path(self.executable).resolve()), '-c', str(self.root/'chat.conf'),
            cwd=self.root, stdout=self.log, stderr=self.log,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
        try:
            await asyncio.wait_for(self.registered.wait(), 15)
        except asyncio.TimeoutError:
            self.log.flush()
            self.fail('Native fixture failed to register: ' + (self.root/'native.log').read_text(errors='replace')[-3000:])
        if self.persistent:
            for attempt in range(150):
                try:
                    await self.exchange('world-a',1,2,presence('a'*64,1,[]))
                    break
                except (asyncio.IncompleteReadError,ConnectionError):
                    pass
                if self.character_task.done():
                    await self.character_task
                    self.fail('Character fixture stopped before persistence readiness')
                await asyncio.sleep(.1)
            else:
                self.fail('Native persistence failed to become ready')

    async def serve_hub(self, reader, writer):
        self.hub_clients.add(writer)
        task = asyncio.current_task()
        self.hub_tasks.add(task)
        try:
            while True:
                magic, version, kind, size = struct.unpack('!4sHHI', await reader.readexactly(12))
                if magic != b'SFHC' or version != 1 or size > 4096:
                    return
                await reader.readexactly(size)
                peer = writer.get_extra_info('peercert')
                identity = next((value for group in peer.get('subject',()) for key,value in group if key=='commonName'), '')
                if kind == 2 and identity == 'chat-test':
                    self.registered.set()
                reply = struct.pack('!HI', kind, 15)
                writer.write(b'SFHC' + struct.pack('!HHI', 1, 0x8000, len(reply)) + reply)
                await writer.drain()
        except (OSError, asyncio.IncompleteReadError):
            pass
        finally:
            writer.close()
            self.hub_clients.discard(writer)
            self.hub_tasks.discard(task)

    async def failure_logs(self):
        result = self._outcome.result if self._outcome else None
        if result and any(test is self for test,_ in result.errors + result.failures):
            for name in ('native.log','standby.log'):
                path = self.root/name
                if path.exists(): print(name + ':\n' + path.read_text(errors='replace')[-6000:],flush=True)

    async def cleanup(self):
        if self.client_process and self.client_process.returncode is None:
            self.client_process.kill()
            await self.client_process.wait()
        if self.standby_process and self.standby_process.returncode is None:
            self.standby_process.kill()
            await self.standby_process.wait()
        if self.standby_log:
            self.standby_log.close()
        if self.process and self.process.returncode is None:
            self.process.terminate()
            try:
                await asyncio.wait_for(self.process.wait(), 5)
            except asyncio.TimeoutError:
                self.process.kill()
                await self.process.wait()
        if self.character_stop:
            self.character_stop.set()
            await asyncio.wait_for(self.character_task,10)
        if self.hub:
            self.hub.close()
            await self.hub.wait_closed()
        for writer in list(self.hub_clients):
            writer.close()
        if self.hub_tasks:
            await asyncio.gather(*list(self.hub_tasks), return_exceptions=True)
        if self.log:
            self.log.close()
        self.temporary.cleanup()

    async def exchange(self, identity, realm, operation, body=b'', accepted=True, port=None):
        tls = ssl.create_default_context(cafile=str(self.root/'ca.pem'))
        tls.load_cert_chain(self.root/(identity+'.pem'), self.root/(identity+'.key'))
        reader, writer = await asyncio.wait_for(asyncio.open_connection('127.0.0.1', self.port if port is None else port,
            ssl=tls, server_hostname='127.0.0.1'),5)
        header = b'SFCH' + struct.pack('!HHII', 1, operation, 1, realm)
        try:
            writer.write(header + (struct.pack('!I', len(body)) + body if operation != 1 else b''))
            await asyncio.wait_for(writer.drain(),4)
            if not accepted:
                self.assertEqual(await asyncio.wait_for(reader.read(1), 4), b'')
                return
            self.assertEqual(await asyncio.wait_for(reader.readexactly(16), 4), header[:6] + struct.pack('!H', 0x8000|operation) + header[8:])
            if operation == 3:
                self.assertEqual(await asyncio.wait_for(reader.readexactly(len(body)),4), body)
            if operation in (4,5,6):
                size = struct.unpack('!I', await asyncio.wait_for(reader.readexactly(4),4))[0]
                self.assertLessEqual(size, 262400)
                return await asyncio.wait_for(reader.readexactly(size),4)
        except (ConnectionResetError, ssl.SSLError):
            if accepted:
                raise
        finally:
            writer.close()
            with contextlib.suppress(OSError, asyncio.TimeoutError):
                await asyncio.wait_for(writer.wait_closed(),2)

    async def restart_native(self):
        self.process.terminate()
        await asyncio.wait_for(self.process.wait(),5)
        self.registered.clear()
        self.process = await asyncio.create_subprocess_exec(str(Path(self.executable).resolve()),'-c',str(self.root/'chat.conf'),
            cwd=self.root,stdout=self.log,stderr=self.log,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform=='win32' else 0)
        await asyncio.wait_for(self.registered.wait(),15)
        for attempt in range(150):
            try:
                await self.exchange('world-a',1,2,presence('a'*64,1,[(10,100,1,'Sender')]))
                break
            except (asyncio.IncompleteReadError,ConnectionError):
                await asyncio.sleep(.1)
        else:
            self.fail('Persistence did not recover after chat restart')
        await self.exchange('world-b',1,2,presence('b'*64,1,[(20,200,1,'Receiver')]))

    async def test_real_listener_realm_identity_and_session_fencing(self):
        generation = 'a'*64
        players = [(10, 100, 1, 'Sender'), (20, 200, 2, 'Officer'), (30, 300, 3, 'Member')]
        await self.exchange('world-a', 1, 1)
        await self.exchange('outsider', 1, 1, accepted=False)
        await self.exchange('world-a', 1, 4, route(generation, 1, 10, [(100, 1, 3)]), accepted=False)
        await self.exchange('world-a', 2, 2, presence(generation, 1, players), accepted=False)
        await self.exchange('world-a', 1, 2, presence(generation, 1, players))
        # Identical character GUIDs may exist in different realms.
        await self.exchange('world-b', 2, 2, presence(generation, 1, players))
        # Replaying presence or stealing a live generation is rejected.
        await self.exchange('world-a', 1, 2, presence(generation, 1, players), accepted=False)
        await self.exchange('world-a', 1, 2, presence('b'*64, 2, players), accepted=False)
        reply = await self.exchange('world-a', 1, 4, route(generation, 2, 6, [(100, 1, 3), (200, 2, 2), (300, 3, 0)]))
        self.assertEqual(reply, struct.pack('!HQQQQ', 2, 100, 1, 200, 2))
        await self.exchange('world-a', 1, 4, route(generation, 2, 6, [(100, 1, 3)]), accepted=False)
        whisper = text(generation) + struct.pack('!IQQQQ', 10, 100, 1, 200, 2) + text('hello')
        await self.exchange('world-a', 1, 3, whisper)
        await self.exchange('world-b', 2, 4, route(generation, 1, 10, [(100, 1, 3)], account=99), accepted=False)
        await self.exchange('world-a', 1, 2, presence(generation, 3, [(10, 100, 4, 'Sender')]))
        await self.exchange('world-a', 1, 3, whisper, accepted=False)


class NativePersistentChatTests(NativeChatTests):
    persistent = True

    def character_database_factory(self, *args, **kwargs):
        from database import CharacterDatabase
        from wire import Reader
        database = CharacterDatabase(*args, **kwargs)
        execute = database.social.execute
        self.lost_reply = dict(armed=False, attempts=0, dropped=False, reconciled=0)

        def intercept(peer, instance, epoch, payload):
            state = self.lost_reply
            mutation = None
            if payload[0] == 18:
                reader = Reader(payload)
                reader.u8()
                mutation = json.loads(reader.blob())
                if (mutation.get('document') or {}).get('motd') == state.get('marker', 'Lost reply fixture'):
                    state['attempts'] += 1
            result = execute(peer, instance, epoch, payload)
            if mutation and state['armed'] and (mutation.get('document') or {}).get('motd') == state.get('marker', 'Lost reply fixture'):
                state['receipt'] = mutation['id']
                state['armed'] = False
                state['dropped'] = True
                raise ConnectionError('Fixture dropped the reply after the database committed')
            if payload[0] == 20 and state['dropped']:
                state['reconciled'] += 1
            return result

        database.social.execute = intercept
        return database

    @classmethod
    def setUpClass(cls):
        if not cls.database_config or not cls.executable:
            raise unittest.SkipTest('Pass --database-config and --chatserver for native persistence integration')
        import character_service_test
        cls.database_fixture = character_service_test.DatabaseTests
        cls.database_fixture.config_path = cls.database_config
        cls.database_fixture.setUpClass()
        from database import CharacterDatabase
        from social_store import canonical
        from wire import u32, blob
        import secrets
        service_path=Path(__file__).resolve().parents[1]/'server'/'characterserver'
        db=CharacterDatabase(cls.database_fixture.config | {'allowed_chat_nodes':['chat-test']},service_path/'statements.json')
        try:
            session,epoch=db.social.attach('chat-test',b'\1'+u32(1)+blob(secrets.token_hex(16))+blob('guilds'))
            guild=dict(id=77,name='FixtureGuild',leader=100,motd='Before',info='',
                       ranks=[dict(name='Leader',rights=255),dict(name='Member',rights=0x43)],
                       members=[dict(guid=100,rank=0,public_note='',officer_note='secret'),dict(guid=200,rank=1,public_note='',officer_note='')])
            db.social.execute('chat-test',session,epoch,b'\x12'+blob(canonical(dict(id=secrets.token_hex(16),key='guild-77',expected=0,actor=100,document=guild))))
            db.social.detach(session)
        finally:
            db.close()

    @classmethod
    def tearDownClass(cls):
        cls.database_fixture.tearDownClass()

    async def test_real_listener_realm_identity_and_session_fencing(self):
        # Both native persistence workers must attach before hub readiness.
        print('Native chat phase: persistent routing',flush=True)
        await self.exchange('world-a',1,2,presence('a'*64,2,[(10,100,1,'Sender')]))
        await self.exchange('world-b',1,2,presence('b'*64,1,[(20,200,1,'Receiver')]))
        await self.exchange('world-b',2,1,accepted=False)
        with self.database_fixture.admin.cursor() as cursor:
            cursor.execute('SELECT domain,node FROM character_social_owners WHERE realm=1 ORDER BY domain')
            self.assertEqual(cursor.fetchall(),(('channels','chat-test'),('guilds','chat-test')))
        async def rpc(domain,sequence,payload,identity='world-a',actor=100,account=10,generation='a'*64):
            response=await self.exchange(identity,1,5,service(domain,generation,sequence,account,actor,payload))
            response_sequence,status,size=struct.unpack('!QBI',response[:13])
            self.assertEqual(response_sequence,sequence)
            self.assertEqual(len(response)-13,size)
            return status,response[13:]
        status,roster=await rpc(2,1,guild_command(18))
        self.assertEqual(status,0)
        revision=struct.unpack('!Q',roster[2:10])[0]
        status,updated=await rpc(2,2,guild_command(0,revision,value='Durably changed'))
        self.assertEqual(status,0)
        with self.database_fixture.admin.cursor() as cursor:
            cursor.execute("SELECT document FROM character_social_records WHERE realm=1 AND domain='guilds' AND record_key='guild-77'")
            self.assertEqual(json.loads(cursor.fetchone()[0])['motd'],'Durably changed')
        status,_=await rpc(2,3,guild_command(0,revision,value='Stale write'))
        self.assertEqual(status,1)
        revision=struct.unpack('!Q',updated[2:10])[0]
        status,_=await rpc(2,1,guild_command(0,revision,value='Unauthorized'),identity='world-b',actor=200,account=20,generation='b'*64)
        self.assertEqual(status,1)
        status,_=await rpc(1,4,channel_command(0))
        self.assertEqual(status,0)
        status,_=await rpc(1,5,channel_command(5,password='fixture-password'))
        self.assertEqual(status,0)
        with self.database_fixture.admin.cursor() as cursor:
            cursor.execute("SELECT document FROM character_social_records WHERE realm=1 AND domain='channels'")
            channel=json.loads(cursor.fetchone()[0])
            self.assertTrue(channel['password_verifier'].startswith('pbkdf2-sha256$'))
            self.assertNotIn('fixture-password',json.dumps(channel))
        from chat_transport_fixture import exercise as transport_exercise, exercise_admin, exercise_guild_messages, exercise_guild_invitation, exercise_lost_reply
        from chat_channel_fixture import exercise as channel_exercise, verify_after_restart
        print('Native chat phase: cross-world transport and guilds',flush=True)
        await transport_exercise(self)
        await exercise_admin(self)
        await exercise_guild_messages(self)
        await exercise_guild_invitation(self)
        print('Native chat phase: channels',flush=True)
        revision = await channel_exercise(self)
        print('Native chat phase: daemon restart',flush=True)
        await self.restart_native()
        await verify_after_restart(self,revision)
        print('Native chat phase: lost reply recovery',flush=True)
        await asyncio.wait_for(exercise_lost_reply(self),60)
        from chat_failover_fixture import exercise as failover_exercise
        print('Native chat phase: two-daemon takeover',flush=True)
        await asyncio.wait_for(failover_exercise(self),100)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--chatserver')
    parser.add_argument('--database-config')
    parser.add_argument('--debug-timeout',type=int,default=0)
    args, remaining = parser.parse_known_args()
    NativeChatTests.executable = args.chatserver
    NativeChatTests.database_config = args.database_config
    if args.debug_timeout:
        import faulthandler
        faulthandler.dump_traceback_later(args.debug_timeout,repeat=True)
    unittest.main(argv=[sys.argv[0]] + remaining)
