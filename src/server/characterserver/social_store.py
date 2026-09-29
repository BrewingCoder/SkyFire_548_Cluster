# This file is part of Project SkyFire https://www.projectskyfire.org.
# See LICENSE.md file for Copyright information
"""Fenced social snapshots, receipts and outbox. Never accepts caller SQL."""
import hashlib
import json
import re
from wire import Reader, blob, u32
from social_projection import project_guild

DOMAINS = ('channels', 'guilds')
MAX_DOCUMENT = 256 * 1024
MAX_REVISION = (1 << 63) - 1


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def integer(value, maximum=MAX_REVISION, minimum=0):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError('Invalid social integer')


def text(value, limit, empty=True):
    if not isinstance(value, str) or len(value.encode()) > limit or (not empty and not value) or any(ord(c) < 32 for c in value):
        raise ValueError('Invalid social text')


def guild_text(value, characters, empty=True, byte_limit=None):
    text(value, byte_limit if byte_limit is not None else characters*3, empty)
    if len(value)>characters or any(ord(char)>0xffff for char in value):
        raise ValueError('Guild text exceeds legacy schema limits')


def fields(value, names):
    if not isinstance(value, dict) or set(value) != set(names.split()):
        raise ValueError('Invalid social fields')


def document(domain, value):
    """Persistence schema only. Chat authority must validate actor permissions."""
    if value is None:
        return None  # Tombstone retains the version, preventing ABA updates.
    if domain == 'channels':
        fields(value, 'name team channel_id announce ownership password_verifier bans runtime' if 'runtime' in value else 'name team channel_id announce ownership password_verifier bans')
        text(value['name'], 128, False)
        integer(value['team'], 0xffffffff)
        integer(value['channel_id'], 0xffffffff)
        for key in ('announce', 'ownership'):
            if type(value[key]) is not bool:
                raise ValueError('Invalid channel setting')
        verifier = value['password_verifier']
        if not isinstance(verifier, str) or (verifier and not re.fullmatch(r'pbkdf2-sha256\$[1-9][0-9]{4,6}\$[0-9a-f]{32}\$[0-9a-f]{64}', verifier)):
            raise ValueError('Password verifier required; plaintext is forbidden')
        bans = value['bans']
        if not isinstance(bans, list) or len(bans) > 4096:
            raise ValueError('Invalid channel bans')
        for guid in bans:
            integer(guid, (1 << 64)-1, 1)
        if len(set(bans)) != len(bans):
            raise ValueError('Duplicate channel ban')
        if 'runtime' in value:
            runtime = value['runtime']
            fields(runtime, 'owner members')
            integer(runtime['owner'], (1 << 64)-1)
            if not isinstance(runtime['members'],list) or len(runtime['members']) > 4096:
                raise ValueError('Invalid channel runtime members')
            members = set()
            owners = []
            for member in runtime['members']:
                fields(member, 'guid incarnation team flags cross_faction node generation name account')
                integer(member['guid'], (1 << 64)-1,1)
                integer(member['incarnation'], (1 << 64)-1,1)
                integer(member['team'],0xffffffff)
                integer(member['account'],0xffffffff,1)
                integer(member['flags'],11)
                if member['flags'] & ~11 or type(member['cross_faction']) is not bool or member['guid'] in members:
                    raise ValueError('Invalid channel runtime member')
                text(member['name'],48,False)
                if not isinstance(member['node'],str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,64}',member['node']) or not isinstance(member['generation'],str) or not re.fullmatch(r'[0-9a-f]{64}',member['generation']):
                    raise ValueError('Invalid channel member identity')
                members.add(member['guid'])
                if member['flags'] & 1: owners.append(member['guid'])
            if owners != ([runtime['owner']] if runtime['owner'] else []):
                raise ValueError('Invalid channel owner flags')
    elif domain == 'guilds':
        fields(value, 'id name leader motd info ranks members')
        integer(value['id'], 0xffffffff, 1)
        integer(value['leader'], (1 << 64)-1, 1)
        guild_text(value['name'], 24, False)
        guild_text(value['motd'], 128)
        guild_text(value['info'], 2047, byte_limit=2047)
        if not isinstance(value['ranks'], list) or not 2 <= len(value['ranks']) <= 10:
            raise ValueError('Invalid guild ranks')
        for rank in value['ranks']:
            fields(rank, 'name rights')
            guild_text(rank['name'], 20, False)
            integer(rank['rights'], 0xffffff)
        members = value['members']
        if not isinstance(members, list) or not 1 <= len(members) <= 4096:
            raise ValueError('Invalid guild members')
        guids = set()
        for member in members:
            fields(member, 'guid rank public_note officer_note')
            integer(member['guid'], (1 << 64)-1, 1)
            integer(member['rank'], len(value['ranks'])-1)
            guild_text(member['public_note'], 31)
            guild_text(member['officer_note'], 31)
            if member['guid'] in guids:
                raise ValueError('Duplicate guild member')
            guids.add(member['guid'])
        if value['leader'] not in guids:
            raise ValueError('Guild leader must be a member')
        if [member['guid'] for member in members if member['rank'] == 0] != [value['leader']]:
            raise ValueError('Guild leader must be its sole rank-zero member')
    else:
        raise ValueError('Invalid social domain')
    result = canonical(value)
    if len(result) > MAX_DOCUMENT:
        raise ValueError('Social snapshot too large')
    return result


def one(cursor):
    result = cursor.fetchone()
    cursor.fetchall()
    return result


def guild_membership(cursor, realm, key, value):
    """Caller holds the guild owner row lock and commits this with the record CAS."""
    if value is not None and key != 'guild-' + str(value['id']):
        raise ValueError('Guild record identity mismatch')
    cursor.execute('DELETE FROM character_social_guild_members WHERE realm=%s AND record_key=%s', (realm, key.encode()))
    if value is not None:
        for member in value['members']:
            cursor.execute('SELECT record_key FROM character_social_guild_members WHERE realm=%s AND guid=%s', (realm, member['guid']))
            if one(cursor):
                raise ValueError('Character already belongs to another guild')
            cursor.execute('INSERT INTO character_social_guild_members(realm,guid,record_key) VALUES(%s,%s,%s)', (realm, member['guid'], key.encode()))



class SocialStore:
    def __init__(self, database):
        self.database = database
        self.sessions = {}

    def remove_character(self, cursor, guid):
        """Called inside the fenced world's character-delete receipt transaction."""
        realm = self.database.realm
        # Character deletion and faction changes already hold this row. Keep the
        # same order explicit here so every guild writer uses world -> social ->
        # projection locks, including future callers using another connection.
        cursor.execute('SELECT realm FROM character_service_owners WHERE realm=%s FOR UPDATE', (realm,))
        one(cursor)
        cursor.execute('SELECT record_key FROM character_social_guild_members WHERE realm=%s AND guid=%s', (realm,guid))
        member = one(cursor)
        if not member:
            return
        key = member[0]
        cursor.execute("SELECT revision FROM character_social_owners WHERE realm=%s AND domain='guilds' FOR UPDATE", (realm,))
        owner = one(cursor)
        if not owner:
            raise ValueError('Guild authority ownership missing')
        cursor.execute("SELECT document FROM character_social_records WHERE realm=%s AND domain='guilds' AND record_key=%s", (realm,key))
        row = one(cursor)
        if not row or row[0] is None:
            raise ValueError('Guild membership projection is inconsistent')
        previous = json.loads(row[0])
        value = json.loads(row[0])
        value['members'] = [member for member in value['members'] if member['guid'] != guid]
        if not value['members']:
            value = None
        elif previous['leader'] == guid:
            successor = min(value['members'],key=lambda member:(member['rank'],member['guid']))
            value['leader'] = successor['guid']
            successor['rank'] = 0
        encoded = document('guilds',value)
        revision = owner[0]+1
        integer(revision)
        guild_membership(cursor,realm,key.decode(),value)
        project_guild(cursor,key.decode(),value,guid,previous=previous,realm=realm)
        cursor.execute("UPDATE character_social_records SET revision=%s,document=%s WHERE realm=%s AND domain='guilds' AND record_key=%s", (revision,encoded,realm,key))
        cursor.execute("INSERT INTO character_social_outbox(realm,domain,revision,record_key,document,actor) VALUES(%s,'guilds',%s,%s,%s,%s)", (realm,revision,key,encoded,guid))
        cursor.execute("UPDATE character_social_owners SET revision=%s WHERE realm=%s AND domain='guilds'", (revision,realm))

    def attach(self, peer, payload):
        reader = Reader(payload)
        version, realm = reader.u8(), reader.u32()
        instance, domain = reader.text(32), reader.text(16)
        reader.end()
        db = self.database
        if peer not in db.config.get('allowed_chat_nodes', []) or version != 1 or realm != db.realm or domain not in DOMAINS or not re.fullmatch('[0-9a-f]{32}', instance):
            raise ValueError('Invalid social identity, realm or protocol')
        with db.lock:
            if db.failed:
                raise RuntimeError('Character database unavailable')
            if self.sessions.get((domain, instance)):
                # A successful reconnect is a barrier: the old connection must
                # detach only after its shielded mutation has finished. Receipt
                # absence is otherwise ambiguous while that mutation is queued.
                raise ValueError('Social incarnation already has an attached session')
            try:
                db.db.begin()
                with db.db.cursor() as cursor:
                    if domain == 'guilds':
                        # Rebuilding the membership index must follow the same
                        # ownership order as character deletion and guild writes.
                        cursor.execute('SELECT realm FROM character_service_owners WHERE realm=%s FOR UPDATE', (realm,))
                        one(cursor)
                    cursor.execute('SELECT instance FROM character_social_retired WHERE realm=%s AND domain=%s AND instance=%s', (realm, domain, instance))
                    if one(cursor):
                        raise ValueError('Retired social incarnation')
                    cursor.execute('SELECT epoch,node,instance FROM character_social_owners WHERE realm=%s AND domain=%s FOR UPDATE', (realm, domain))
                    owner = one(cursor)
                    if owner and owner[2] == instance and owner[1] != peer:
                        raise ValueError('Social incarnation belongs to another identity')
                    if owner and owner[1:] != (peer, instance):
                        if any(key[0] == domain for key in self.sessions):
                            raise ValueError('Social domain still has attached owner')
                        cursor.execute('INSERT IGNORE INTO character_social_retired VALUES(%s,%s,%s)', (realm, domain, owner[2]))
                    epoch = owner[0] if owner and owner[1:] == (peer, instance) else (owner[0]+1 if owner else 1)
                    if domain == 'guilds' and not self.sessions.get((domain, instance)):
                        # Upgrade existing staged records under the same ownership lock.
                        # Any invalid/duplicate membership aborts attachment atomically.
                        cursor.execute('SELECT record_key,document FROM character_social_records WHERE realm=%s AND domain=%s LIMIT 8193', (realm, domain))
                        records = cursor.fetchall()
                        if len(records) > 8192:
                            raise ValueError('Guild snapshot exceeds cache bounds')
                        cursor.execute('DELETE FROM character_social_guild_members WHERE realm=%s', (realm,))
                        for key, data in records:
                            value = json.loads(data) if data is not None else None
                            document(domain, value)
                            guild_membership(cursor, realm, key.decode(), value)
                    cursor.execute('INSERT INTO character_social_owners(realm,domain,epoch,node,instance) VALUES(%s,%s,%s,%s,%s) ON DUPLICATE KEY UPDATE epoch=VALUES(epoch),node=VALUES(node),instance=VALUES(instance)', (realm, domain, epoch, peer, instance))
                db.db.commit()
                session = (domain, instance)
                self.sessions[session] = self.sessions.get(session, 0)+1
                return session, epoch
            except BaseException as error:
                db._rollback(error)
                raise

    def detach(self, session):
        with self.database.lock:
            if session in self.sessions:
                self.sessions[session] -= 1
                if not self.sessions[session]:
                    del self.sessions[session]

    def execute(self, peer, session, epoch, payload):
        reader = Reader(payload)
        operation = reader.u8()
        raw = reader.blob(MAX_DOCUMENT+4096)
        reader.end()
        def unique(items):
            result = {}
            for key, value in items:
                if key in result:
                    raise ValueError('Duplicate social field')
                result[key] = value
            return result
        request = json.loads(raw, object_pairs_hook=unique)
        domain, instance = session
        if operation == 17:
            fields(request, 'after limit')
            text(request['after'], 192)
            integer(request['limit'], 32, 1)
        elif operation == 18:
            fields(request, 'id key expected actor document context' if 'context' in request else 'id key expected actor document')
            if 'context' in request and (domain != 'guilds' or not isinstance(request['context'], dict)):
                raise ValueError('Invalid social context')
            if not isinstance(request['id'], str) or not re.fullmatch('[0-9a-f]{32}', request['id']):
                raise ValueError('Invalid social request identity')
            text(request['key'], 192, False)
            integer(request['expected'])
            context = request.get('context',{})
            console = domain == 'guilds' and context.get('console') is True and type(context.get('admin_permission')) is int and 402 <= context['admin_permission'] <= 407
            integer(request['actor'], (1 << 64)-1, 0 if console else 1)
            encoded = document(domain, request['document'])
        elif operation == 19:
            fields(request, 'after limit')
            integer(request['after'])
            integer(request['limit'], 32, 1)
        elif operation == 20:
            fields(request, 'id')
            if not isinstance(request['id'], str) or not re.fullmatch('[0-9a-f]{32}', request['id']):
                raise ValueError('Invalid social receipt identity')
        else:
            raise ValueError('Unsupported social operation')
        db = self.database
        with db.lock:
            if db.failed or not self.sessions.get(session):
                raise RuntimeError('Social session unavailable')
            try:
                db.db.begin()
                with db.db.cursor() as cursor:
                    if operation == 18 and domain == 'guilds':
                        # Petition consumption consults the fenced world owner.
                        # Taking it after the social owner reverses the lock order
                        # used by world character/faction-change transactions.
                        # Absence is allowed for administrative offline mutations.
                        cursor.execute('SELECT realm FROM character_service_owners WHERE realm=%s FOR UPDATE', (db.realm,))
                        one(cursor)
                    cursor.execute('SELECT epoch,node,instance,revision FROM character_social_owners WHERE realm=%s AND domain=%s FOR UPDATE', (db.realm, domain))
                    owner = one(cursor)
                    if not owner or owner[:3] != (epoch, peer, instance):
                        raise ValueError('Social owner fenced')
                    if operation == 17:
                        cursor.execute('SELECT record_key,revision,document FROM character_social_records WHERE realm=%s AND domain=%s AND record_key>%s ORDER BY record_key LIMIT %s', (db.realm, domain, request['after'].encode(), request['limit']))
                        items = cursor.fetchall()
                        result = {'head': owner[3], 'records': [{'key': key.decode(), 'revision': revision, 'document': json.loads(data) if data is not None else None} for key, revision, data in items]}
                    elif operation == 19:
                        cursor.execute('SELECT revision,record_key,document,actor FROM character_social_outbox WHERE realm=%s AND domain=%s AND revision>%s ORDER BY revision LIMIT %s', (db.realm, domain, request['after'], request['limit']))
                        result = {'head': owner[3], 'events': [{'revision': revision, 'key': key.decode(), 'document': json.loads(data) if data is not None else None, 'actor': actor} for revision, key, data, actor in cursor.fetchall()]}
                    elif operation == 20:
                        # The same social ownership fence protects receipt lookup.
                        # Never expose an earlier incarnation's receipt as proof
                        # that the current incarnation committed a mutation.
                        cursor.execute('SELECT revision FROM character_social_receipts WHERE realm=%s AND domain=%s AND request_id=%s AND instance=%s',
                                       (db.realm, domain, request['id'], instance))
                        receipt = one(cursor)
                        result = {'found': True, 'revision': receipt[0]} if receipt else {'found': False}
                    else:
                        digest = hashlib.sha256(canonical(request)).hexdigest()
                        cursor.execute('SELECT instance,digest,revision FROM character_social_receipts WHERE realm=%s AND domain=%s AND request_id=%s', (db.realm, domain, request['id']))
                        receipt = one(cursor)
                        if receipt:
                            if receipt[:2] != (instance, digest):
                                raise ValueError('Social receipt identity reused')
                            result = {'revision': receipt[2]}
                        else:
                            cursor.execute('SELECT revision,document FROM character_social_records WHERE realm=%s AND domain=%s AND record_key=%s', (db.realm, domain, request['key'].encode()))
                            old = one(cursor)
                            if (old[0] if old else 0) != request['expected']:
                                raise ValueError('Social record revision conflict')
                            revision = owner[3]+1
                            integer(revision)
                            if domain == 'guilds':
                                guild_membership(cursor, db.realm, request['key'], request['document'])
                                if db.config.get('social_guild_projection', False):
                                    project_guild(cursor, request['key'], request['document'], request['actor'], request.get('context'), json.loads(old[1]) if old and old[1] is not None else None, db.realm)
                            cursor.execute('INSERT INTO character_social_records VALUES(%s,%s,%s,%s,%s) ON DUPLICATE KEY UPDATE revision=VALUES(revision),document=VALUES(document)', (db.realm, domain, request['key'].encode(), revision, encoded))
                            cursor.execute('INSERT INTO character_social_outbox VALUES(%s,%s,%s,%s,%s,%s)', (db.realm, domain, revision, request['key'].encode(), encoded, request['actor']))
                            cursor.execute('INSERT INTO character_social_receipts VALUES(%s,%s,%s,%s,%s,%s)', (db.realm, domain, request['id'], instance, digest, revision))
                            cursor.execute('UPDATE character_social_owners SET revision=%s WHERE realm=%s AND domain=%s', (revision, db.realm, domain))
                            result = {'revision': revision}
                answer = canonical(result)
                db.db.commit()
                return answer
            except BaseException as error:
                db._rollback(error)
                raise
