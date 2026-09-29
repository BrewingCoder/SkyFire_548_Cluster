# This file is part of Project SkyFire https://www.projectskyfire.org.
# See LICENSE.md file for Copyright information
"""Cross-world delivery checks against NativePersistentChatTests' real TLS listener."""
import asyncio
import json
import struct


async def exercise_lost_reply(fixture):
    """Commit on real SQL, close the TLS reply, then recover the exact receipt."""
    await _refresh(fixture)
    async def rpc(sequence, action, revision=0, value='', target=0):
        payload = struct.pack('!BIQBQQII', 1, 77, revision, action, target, 0, 0, 0) + string(value)
        response = await fixture.exchange('world-a', 1, 5, service(2, 'a'*64, sequence, 10, 100, payload))
        number, status, size = struct.unpack('!QBI', response[:13])
        fixture.assertEqual(number, sequence)
        fixture.assertEqual(size, len(response)-13)
        return status, response[13:]
    status, roster = await rpc(10000, 18)
    fixture.assertEqual(status, 0)
    before = struct.unpack('!Q', roster[2:10])[0]
    fixture.lost_reply['armed'] = True
    try:
        status, _ = await rpc(10001, 0, before, 'Lost reply fixture')
        fixture.assertIn(status, (0, 2, 3))
    except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError):
        pass  # The committed mutation deliberately loses its transport response.
    fixture.assertTrue(fixture.lost_reply['dropped'])
    recovered = False
    for attempt in range(30):
        if attempt % 5 == 0:
            print('Lost reply reconciliation attempt',attempt,'receipt queries',fixture.lost_reply['reconciled'],flush=True)
        await asyncio.sleep(0.5)
        try:
            await _refresh(fixture)
            status, roster = await rpc(10002 + attempt, 18)
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError):
            continue  # Persistence reconnect deliberately marks the listener unavailable.
        if status == 0 and fixture.lost_reply['reconciled']:
            fixture.assertEqual(struct.unpack('!Q', roster[2:10])[0], before + 1)
            recovered = True
            break
    fixture.assertTrue(recovered, 'Committed projection must recover after a lost reply')
    status, recovered_reply = await rpc(10040, 21, target=10001)
    fixture.assertEqual(status, 0, 'Status lookup must recover the original committed response')
    fixture.assertEqual(struct.unpack('!Q', recovered_reply[2:10])[0], before + 1)
    fixture.assertEqual(fixture.lost_reply['attempts'], 1, 'Never replay an ambiguous mutation')
    with fixture.database_fixture.admin.cursor() as cursor:
        cursor.execute("SELECT revision,document FROM character_social_records WHERE realm=1 AND domain='guilds' AND record_key='guild-77'")
        revision, document = cursor.fetchone()
        fixture.assertEqual(revision, before + 1)
        fixture.assertEqual(json.loads(document)['motd'], 'Lost reply fixture')
        cursor.execute('SELECT COUNT(*) FROM character_social_receipts WHERE request_id=%s', (fixture.lost_reply['receipt'],))
        fixture.assertEqual(cursor.fetchone()[0], 1)


def string(value):
    value = value.encode() if isinstance(value, str) else value
    return struct.pack('!H', len(value)) + value


def blob(value):
    value = value.encode() if isinstance(value, str) else value
    return struct.pack('!I', len(value)) + value


def service(domain, generation, sequence, account, actor, payload, incarnation=1):
    return bytes([domain]) + string(generation) + struct.pack('!QIQQ', sequence, account, actor, incarnation) + blob(payload)


def whisper(token=0):
    return (struct.pack('!BQQQIBBB', 1, token, 100, 1, 469, 80, 0, 0) +
            blob('Sender') + blob('Receiver') + blob('remote hello') + blob(''))


def group():
    return (struct.pack('!QQQQBIBBB', 123, 100, 1, 0, 1, 0, 255, 0, 0) +
            blob('Sender') + blob('group hello') + blob('') + struct.pack('!HQQ', 2, 100, 200))


async def _refresh(fixture):
    sequence = getattr(fixture, '_transport_presence_sequence', 10) + 1
    fixture._transport_presence_sequence = sequence
    for world, generation, account, guid, name in [('world-a','a'*64,10,100,'Sender'),('world-b','b'*64,20,200,'Receiver')]:
        body = (string(generation)+struct.pack('!QH',sequence,1)+struct.pack('!IQQ',account,guid,1)+string(name)+
                struct.pack('!BHQIBB',1,1,guid,469,0,7))
        await fixture.exchange(world,1,2,body)


async def exercise(fixture):
    """Call after worlds a/b own GUID100/200 (account10/20/incarnation1) in realm1."""
    await _refresh(fixture)
    generations = {'world-a': 'a'*64, 'world-b': 'b'*64}
    epochs = {'world-a': '0'*64, 'world-b': '0'*64}
    cursors = {'world-a': 0, 'world-b': 0}

    async def rpc(world, sequence, domain, payload, accepted=True, account=None, actor=None, incarnation=1, realm=1):
        default = (10, 100) if world == 'world-a' else (20, 200)
        response = await fixture.exchange(world, realm, 5, service(domain, generations[world], sequence,
            default[0] if account is None else account, default[1] if actor is None else actor, payload, incarnation), accepted=accepted)
        if not accepted:
            return None
        number, status, length = struct.unpack('!QBI', response[:13])
        fixture.assertEqual(number, sequence)
        fixture.assertEqual(length, len(response)-13)
        return status, response[13:]

    async def poll(world, acknowledge=True):
        body = string(generations[world]) + struct.pack('!Q', cursors[world]) + string(epochs[world])
        response = await fixture.exchange(world, 1, 6, body)
        size = struct.unpack('!H', response[:2])[0]
        epoch = response[2:2+size].decode()
        fixture.assertEqual(len(epoch), 64)
        offset = 2 + size
        cursor, count = struct.unpack('!QH', response[offset:offset+10]); offset += 10
        previous = cursors[world] if epoch == epochs[world] else 0
        fixture.assertEqual(cursor-previous, count)
        events = []
        for _ in range(count):
            domain, recipient, incarnation, length = struct.unpack('!BQQI', response[offset:offset+21]); offset += 21
            payload = response[offset:offset+length]; offset += length
            events.append((domain, recipient, incarnation, payload))
        fixture.assertEqual(offset, len(response))
        if acknowledge:
            epochs[world], cursors[world] = epoch, cursor
        return events, response

    # Drain any guild/channel projection notifications from the preceding fixture.
    for world in generations:
        for _ in range(8):
            events, _ = await poll(world)
            if not events:
                break
        else:
            fixture.fail('Fixture did not drain its bounded event mailbox')

    status, _ = await rpc('world-a', 100, 3, whisper())
    fixture.assertEqual(status, 0)
    offers, first = await poll('world-b', acknowledge=False)
    repeated, second = await poll('world-b', acknowledge=False)
    fixture.assertEqual(first, second, 'Unacknowledged event page must replay identically')
    offers, _ = await poll('world-b')
    offer = next(event for event in offers if event[0] == 3)
    fixture.assertEqual(offer[1:3], (200, 1))
    token = struct.unpack('!Q', offer[3][1:9])[0]
    fixture.assertGreater(token, 0)
    reply = struct.pack('!BQQBBBB', 2, token, 200, 0, 80, 0, 0) + blob('Receiver') + blob('')
    # A sender cannot forge the recipient world's delivery acknowledgment.
    status, _ = await rpc('world-a', 101, 3, reply)
    fixture.assertEqual(status, 1)
    status, _ = await rpc('world-b', 100, 3, reply)
    fixture.assertEqual(status, 0)
    outcomes, _ = await poll('world-a')
    outcome = next(event for event in outcomes if event[0] == 3)
    fixture.assertEqual(outcome[1:3], (100, 1))
    fixture.assertEqual(struct.unpack('!Q', outcome[3][1:9])[0], 100, 'Reply must carry source request sequence')
    status, _ = await rpc('world-b', 101, 3, reply)
    fixture.assertEqual(status, 1, 'Duplicate recipient acknowledgment must not deliver twice')
    await rpc('world-a', 102, 3, whisper(), accepted=False, account=99)
    await rpc('world-a', 102, 3, whisper(), accepted=False, incarnation=2)
    await rpc('world-a', 102, 3, whisper(), accepted=False, realm=2)

    status, _ = await rpc('world-a', 102, 4, group())
    fixture.assertEqual(status, 0)
    for world, guid in (('world-a', 100), ('world-b', 200)):
        events, _ = await poll(world)
        delivered = [event for event in events if event[0] == 4]
        fixture.assertEqual(len(delivered), 1)
        fixture.assertEqual(delivered[0][1:3], (guid, 1))
        fixture.assertEqual(delivered[0][3], group())
    await rpc('world-a', 102, 4, group(), accepted=False)
    for world in generations:
        events, _ = await poll(world)
        fixture.assertFalse(any(event[0] == 4 for event in events))

    # Transient messages retained while a destination is stalled expire rather
    # than appearing late. The empty event advances the acknowledgment cursor.
    status, _ = await rpc('world-a', 103, 4, group())
    fixture.assertEqual(status, 0)
    await asyncio.sleep(4.2)
    for world in generations:
        events, _ = await poll(world)
        expired = [event for event in events if event[0] == 4]
        fixture.assertEqual(len(expired), 1)
        fixture.assertEqual(expired[0][3], b'')


async def exercise_admin(fixture):
    """Run after fixture's other guild assertions; restore the guild name afterward."""
    await _refresh(fixture)
    async def rpc(sequence, action, revision=0, name='', operation=6, permission=407):
        payload = (struct.pack('!BIQBQQII', 1, 77, revision, action, 0, 0, 0, 0) + string(name) +
                   struct.pack('!BIB', operation, permission, 1))
        response = await fixture.exchange('world-a', 1, 5, service(2, 'a'*64, sequence, 0, 0, payload, 0))
        number, status, size = struct.unpack('!QBI', response[:13])
        fixture.assertEqual(number, sequence)
        fixture.assertEqual(size, len(response)-13)
        return status, response[13:]
    status, _ = await rpc(200, 18, permission=406)
    fixture.assertEqual(status, 1, 'Console permission attestation must match its typed operation')
    status, _ = await rpc(201, 18, operation=0, permission=0)
    fixture.assertEqual(status, 1, 'Actorless generic guild request must not be accepted')
    status, roster = await rpc(202, 18)
    fixture.assertEqual(status, 0)
    revision = struct.unpack('!Q', roster[2:10])[0]
    with fixture.database_fixture.admin.cursor() as cursor:
        cursor.execute("SELECT document FROM character_social_records WHERE realm=1 AND domain='guilds' AND record_key='guild-77'")
        original = json.loads(cursor.fetchone()[0])['name']
    status, renamed = await rpc(203, 1, revision, 'ConsoleFixture')
    fixture.assertEqual(status, 0)
    with fixture.database_fixture.admin.cursor() as cursor:
        cursor.execute("SELECT document FROM character_social_records WHERE realm=1 AND domain='guilds' AND record_key='guild-77'")
        fixture.assertEqual(json.loads(cursor.fetchone()[0])['name'], 'ConsoleFixture')
    revision = struct.unpack('!Q', renamed[2:10])[0]
    status, _ = await rpc(204, 1, revision, original)
    fixture.assertEqual(status, 0)


async def exercise_guild_messages(fixture):
    """Guild/officer/addon routing uses the durable roster across two worlds."""
    await _refresh(fixture)
    cursors = {'world-a': (0, '0'*64), 'world-b': (0, '0'*64)}
    async def poll(world):
        cursor, epoch = cursors[world]
        generation = ('a' if world == 'world-a' else 'b')*64
        response = await fixture.exchange(world, 1, 6, string(generation)+struct.pack('!Q',cursor)+string(epoch))
        size = struct.unpack('!H',response[:2])[0]
        epoch = response[2:2+size].decode(); offset = 2+size
        cursor,count = struct.unpack('!QH',response[offset:offset+10]); offset += 10
        cursors[world] = cursor,epoch
        events = []
        for _ in range(count):
            domain,recipient,incarnation,size = struct.unpack('!BQQI',response[offset:offset+21]); offset += 21
            payload = response[offset:offset+size]; offset += size
            if domain == 2 and payload: events.append((recipient,incarnation,payload))
        fixture.assertEqual(offset,len(response))
        return events,count
    for world in cursors:
        for _ in range(8):
            _,count = await poll(world)
            if not count: break
        else: fixture.fail('Guild fixture mailbox did not drain')

    async def send(world,sequence,officer=False,message='guild hello',language=0,prefix=b''):
        actor,account,generation = (100,10,'a'*64) if world=='world-a' else (200,20,'b'*64)
        payload = (struct.pack('!BIQBQQII',1,77,0,17 if officer else 16,0,0,0,0)+string(message)+
            struct.pack('!IB',language,0)+string(prefix)+struct.pack('!BIB',0,0,0))
        response = await fixture.exchange(world,1,5,service(2,generation,sequence,account,actor,payload))
        number,status,size=struct.unpack('!QBI',response[:13])
        fixture.assertEqual((number,size),(sequence,len(response)-13))
        return status

    def check_message(event,recipient,sender,officer,text,language=0,prefix=b''):
        fixture.assertEqual(event[:2],(recipient,1))
        data=event[2]
        version,guild,lang,source,incarnation,tag,is_officer=struct.unpack('!BIIQQBB',data[:27])
        fixture.assertEqual((version,guild,lang,source,incarnation,is_officer),(1,77,language,sender,1,int(officer)))
        offset=27; strings=[]
        for _ in range(3):
            length=struct.unpack('!H',data[offset:offset+2])[0];offset+=2
            strings.append(data[offset:offset+length]);offset+=length
        fixture.assertEqual(offset,len(data))
        fixture.assertEqual(strings,[b'Sender' if sender==100 else b'Receiver',text.encode() if isinstance(text,str) else text,prefix])

    fixture.assertEqual(await send('world-a',300),0)
    for world,guid in [('world-a',100),('world-b',200)]:
        events,_=await poll(world);fixture.assertEqual(len(events),1)
        check_message(events[0],guid,100,False,'guild hello')
    fixture.assertEqual(await send('world-a',301,officer=True,message='officer secret'),0)
    events,_=await poll('world-a');fixture.assertEqual(len(events),1)
    check_message(events[0],100,100,True,'officer secret')
    events,_=await poll('world-b');fixture.assertFalse(events,'Nonofficer must not receive officer messages')
    fixture.assertEqual(await send('world-b',300,officer=True),1)
    for world in cursors:
        events,_=await poll(world);fixture.assertFalse(events)
    fixture.assertEqual(await send('world-b',301,message='member hello'),0)
    for world,guid in [('world-a',100),('world-b',200)]:
        events,_=await poll(world);fixture.assertEqual(len(events),1)
        check_message(events[0],guid,200,False,'member hello')
    raw=b'\x00\xffbinary'
    fixture.assertEqual(await send('world-a',302,message=raw,language=0xffffffff,prefix=b'TEST'),0)
    for world,guid in [('world-a',100),('world-b',200)]:
        events,_=await poll(world);fixture.assertEqual(len(events),1)
        check_message(events[0],guid,100,False,raw,0xffffffff,b'TEST')
    fixture.assertEqual(await send('world-a',303,message='invalid addon',language=0xffffffff),1)


async def exercise_guild_invitation(fixture):
    """Invite a remote nonmember and reject acceptance from a later login."""
    await _refresh(fixture)
    async def publish(incarnation):
        fixture._transport_presence_sequence += 1
        players = [(20,200,1,'Receiver'), (30,300,incarnation,'Invitee')]
        body = string('b'*64) + struct.pack('!QH',fixture._transport_presence_sequence,2)
        for account,guid,inc,name in players:
            body += struct.pack('!IQQ',account,guid,inc) + string(name)
        body += struct.pack('!BH',1,2)
        for _,guid,_,_ in players:
            body += struct.pack('!QIBB',guid,469,0,7)
        await fixture.exchange('world-b',1,2,body)
    async def rpc(world, sequence, action, revision=0, value='', incarnation=1):
        actor,account,generation = (100,10,'a'*64) if world=='world-a' else (300,30,'b'*64)
        command = struct.pack('!BIQBQQII',1,77,revision,action,0,0,0,0)+string(value)
        response = await fixture.exchange(world,1,5,service(2,generation,sequence,account,actor,command,incarnation))
        number,status,size = struct.unpack('!QBI',response[:13])
        fixture.assertEqual((number,size),(sequence,len(response)-13))
        return status,response[13:]
    await publish(1)
    status,roster = await rpc('world-a',400,18)
    fixture.assertEqual(status,0)
    revision = struct.unpack('!Q',roster[2:10])[0]
    status,_ = await rpc('world-a',401,14,revision,'Invitee')
    fixture.assertEqual(status,0)
    cursor,epoch,found = 0,'0'*64,False
    for _ in range(8):
        response = await fixture.exchange('world-b',1,6,string('b'*64)+struct.pack('!Q',cursor)+string(epoch))
        size = struct.unpack('!H',response[:2])[0]
        epoch = response[2:2+size].decode(); offset = 2+size
        cursor,count = struct.unpack('!QH',response[offset:offset+10]); offset += 10
        for _ in range(count):
            domain,guid,inc,size = struct.unpack('!BQQI',response[offset:offset+21]); offset += 21
            payload = response[offset:offset+size]; offset += size
            if domain == 2 and guid == 300 and payload:
                fixture.assertEqual(inc,1)
                fixture.assertEqual(payload,struct.pack('!BIIQQ',2,77,469,100,1)+string('Sender'))
                found = True
        if not count: break
    fixture.assertTrue(found,'Cross-world invitation must reach the recipient mailbox')
    await publish(2)
    status,_ = await rpc('world-b',400,15,revision,incarnation=2)
    fixture.assertEqual(status,1,'An invitation cannot be accepted by a different login incarnation')
    await _refresh(fixture)
