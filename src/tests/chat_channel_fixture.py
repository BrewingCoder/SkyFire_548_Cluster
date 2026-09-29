# This file is part of Project SkyFire https://www.projectskyfire.org.
# See LICENSE.md file for Copyright information
"""Native channel authority lifecycle through real mTLS and durable persistence."""
import hashlib
import json
import struct


def text(value):
    data = value.encode()
    return struct.pack('!H', len(data)) + data


def optional(value):
    return b'\1' + text(value) if value else b'\0'


def command(action, *, password='', target=0, target_name='', message='', value=False):
    return (bytes([action]) + text('ChannelLifecycle') + struct.pack('!IIIIIQB', 0, 469, 469, 469, 0, target, 16 if value else 0) +
            optional(password) + optional(target_name) + optional(message))


class Reader:
    def __init__(self, data):
        self.data, self.position = data, 0

    def unpack(self, fmt):
        size = struct.calcsize(fmt)
        values = struct.unpack_from(fmt, self.data, self.position)
        self.position += size
        return values

    def string(self):
        size, = self.unpack('!H')
        value = self.data[self.position:self.position + size].decode()
        self.position += size
        return value

    def optional(self):
        present, = self.unpack('!B')
        return self.string() if present else ''


def update(data):
    value = Reader(data)
    name = value.string()
    channel, team, language, actor_team = value.unpack('!IIII')
    revision, owner, actor, removed = value.unpack('!QQQQ')
    error, action, flags = value.unpack('!BBB')
    target_name, message = value.optional(), value.optional()
    count, = value.unpack('!H')
    members = {}
    for _ in range(count):
        guid, incarnation, member_flags, security, profile = value.unpack('!QQBBB')
        members[guid] = dict(incarnation=incarnation, flags=member_flags, security=security, profile=profile)
    count, = value.unpack('!H')
    notices = [value.unpack('!BQQBB') for _ in range(count)]
    if value.position < len(data):
        value.unpack('!B')  # Authenticated sender's chat tag, appended protocol extension.
    if value.position != len(data):
        raise AssertionError('Trailing channel update bytes')
    return dict(name=name, revision=revision, owner=owner, actor=actor, removed=removed,
                error=error, action=action, flags=flags, members=members, notices=notices, text=message, target_name=target_name)


async def exercise(fixture):
    sequences = {'world-a': 2000, 'world-b': 2000}
    profiles = {'world-a': ('a' * 64, 10, 100), 'world-b': ('b' * 64, 20, 200)}

    async def rpc(identity, action, **kwargs):
        generation, account, actor = profiles[identity]
        sequence = sequences[identity]
        sequences[identity] += 1
        payload = command(action, **kwargs)
        envelope = b'\1' + text(generation) + struct.pack('!QIQQI', sequence, account, actor, 1, len(payload)) + payload
        response = await fixture.exchange(identity, 1, 5, envelope)
        received, status, size = struct.unpack('!QBI', response[:13])
        fixture.assertEqual((received, status, size), (sequence, 0, len(response) - 13))
        return update(response[13:]) if response[13:] else None

    cursors = {'world-a': (0, '0' * 64), 'world-b': (0, '0' * 64)}

    async def find_event(identity, predicate):
        generation, _, _ = profiles[identity]
        for _ in range(8):
            cursor, epoch = cursors[identity]
            response = await fixture.exchange(identity, 1, 6, text(generation) + struct.pack('!Q', cursor) + text(epoch))
            reader = Reader(response)
            epoch = reader.string()
            cursor, count = reader.unpack('!QH')
            cursors[identity] = cursor, epoch
            for _ in range(count):
                domain, recipient, incarnation, size = reader.unpack('!BQQI')
                data = response[reader.position:reader.position + size]
                reader.position += size
                if domain == 1 and data:
                    decoded = update(data)
                    if decoded['name'] == 'ChannelLifecycle' and predicate(decoded):
                        return decoded
        fixture.fail('Expected channel event was not delivered')

    first = await rpc('world-a', 0)
    fixture.assertEqual(first['error'], 0)
    fixture.assertEqual(first['owner'], 100)
    second = await rpc('world-b', 0)
    fixture.assertEqual(second['error'], 0)
    fixture.assertEqual(set(second['members']), {100, 200})
    fixture.assertEqual((await rpc('world-b', 5, password='unauthorized'))['error'], 5)
    muted = await rpc('world-a', 8, target=200, target_name='Receiver', value=True)
    fixture.assertEqual(muted['members'][200]['flags'] & 8, 8)
    fixture.assertEqual((await rpc('world-b', 13, message='must not deliver'))['error'], 9)
    await rpc('world-a', 8, target=200, target_name='Receiver', value=False)
    await rpc('world-a', 7, target=200, target_name='Receiver', value=True)
    fixture.assertEqual((await rpc('world-b', 5, password='durable-private-password'))['error'], 0)
    await rpc('world-b', 1)
    fixture.assertIsNone(await rpc('world-a', 12, target_name='Receiver'))
    offer = await find_event('world-b', lambda item: item['action'] == 16)
    fixture.assertEqual(len(offer['text']), 32)
    fixture.assertIsNone(await rpc('world-b', 15, message=offer['text'], value=True))
    outcome = await find_event('world-a', lambda item: item['action'] == 12 and any(notice[0] == 0x1d for notice in item['notices']))
    fixture.assertEqual(outcome['error'], 0)
    fixture.assertEqual((await rpc('world-b', 0, password='wrong'))['error'], 4)
    fixture.assertEqual((await rpc('world-b', 0, password='durable-private-password'))['error'], 0)
    banned = await rpc('world-a', 3, target=200, target_name='Receiver')
    fixture.assertNotIn(200, banned['members'])
    fixture.assertEqual((await rpc('world-b', 0, password='durable-private-password'))['error'], 3)
    await rpc('world-a', 4, target=200, target_name='Receiver')
    await rpc('world-b', 0, password='durable-private-password')
    changed = await rpc('world-a', 9, target=200, target_name='Receiver')
    fixture.assertEqual(changed['owner'], 200)
    owner = await rpc('world-a', 10)
    fixture.assertEqual(owner['target_name'], 'Receiver')
    fixture.assertEqual(owner['revision'], changed['revision'])
    key = 'channel-' + hashlib.sha256(b'469:channellifecycle').hexdigest()
    with fixture.database_fixture.admin.cursor() as cursor:
        cursor.execute("SELECT document FROM character_social_records WHERE realm=1 AND domain='channels' AND record_key=%s", (key,))
        stored = json.loads(cursor.fetchone()[0])
    fixture.assertEqual(stored['runtime']['owner'], 200)
    fixture.assertEqual({member['guid'] for member in stored['runtime']['members']}, {100, 200})
    fixture.assertEqual(stored['bans'], [])
    fixture.assertNotIn('durable-private-password', json.dumps(stored))
    return changed['revision']


async def verify_after_restart(fixture, previous_revision):
    payload = command(11)
    sequence = 3000
    envelope = b'\1' + text('b' * 64) + struct.pack('!QIQQI', sequence, 20, 200, 1, len(payload)) + payload
    response = await fixture.exchange('world-b', 1, 5, envelope)
    received, status, size = struct.unpack('!QBI', response[:13])
    fixture.assertEqual((received, status, size), (sequence, 0, len(response) - 13))
    restored = update(response[13:])
    fixture.assertEqual(restored['error'], 0)
    fixture.assertEqual(restored['owner'], 200)
    fixture.assertEqual(set(restored['members']), {100, 200})
    fixture.assertGreaterEqual(restored['revision'], previous_revision)
