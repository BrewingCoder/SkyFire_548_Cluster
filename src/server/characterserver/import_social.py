# This file is part of Project SkyFire https://www.projectskyfire.org.
# See LICENSE.md file for Copyright information
"""Offline, transactional staging of legacy social data. Never activates ownership."""
import argparse
import hashlib
import json
from pathlib import Path
import secrets
import sys
import tomllib
import unicodedata
from social_store import document, integer

MAX_RECORDS = 8192
MAX_BYTES = 4 * 1024 * 1024
SOURCE_TABLES = ('channels', 'guild', 'guild_rank', 'guild_member')
DESTINATION_TABLES = tuple('character_social_' + name for name in ('owners', 'records', 'outbox', 'receipts', 'retired'))


def channel_key(name, team):
    # Realm is the outer SQL partition; team remains part of channel identity.
    value = str(team) + ':' + unicodedata.normalize('NFC', name).casefold()
    return 'channel-' + hashlib.sha256(value.encode()).hexdigest()


def password_verifier(password):
    if not password:
        return ''
    if not isinstance(password, str):
        raise ValueError('Invalid legacy channel password')
    salt = secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac('sha256', password.encode(), salt, 210000)
    return 'pbkdf2-sha256$210000$' + salt.hex() + '$' + digest.hex()


def channel_document(row):
    name, team, announce, ownership, password, banned = row
    if announce not in (0, 1) or ownership not in (0, 1):
        raise ValueError('Legacy channel has an invalid boolean')
    bans = []
    for item in (banned or '').split():
        if not item.isascii() or not item.isdecimal():
            raise ValueError('Legacy channel ban list contains an invalid GUID')
        guid = int(item)
        integer(guid, (1 << 64) - 1, 1)
        bans.append(guid)
    result = dict(name=name, team=team, channel_id=0, announce=bool(announce), ownership=bool(ownership),
                  password_verifier=password_verifier(password), bans=sorted(set(bans)))
    document('channels', result)
    return result


def guild_document(row, ranks, members):
    guild_id, name, leader, motd, info = row
    if [rank[0] for rank in ranks] != list(range(len(ranks))):
        raise ValueError('Legacy guild ranks must be contiguous starting at zero')
    result = dict(id=guild_id, name=name, leader=leader, motd=motd, info=info,
                  ranks=[dict(name=name, rights=rights) for _, name, rights in ranks],
                  members=[dict(guid=guid, rank=rank, public_note=pnote, officer_note=onote)
                           for guid, rank, pnote, onote in members])
    document('guilds', result)
    if sum(member['rank'] == 0 for member in result['members']) != 1 or not any(
            member['guid'] == leader and member['rank'] == 0 for member in result['members']):
        raise ValueError('Legacy guild leader must be its sole rank-zero member')
    return result


def load_records(cursor):
    records = {'channels': [], 'guilds': []}
    cursor.execute('SELECT name,team,announce,ownership,password,bannedList FROM channels ORDER BY team,name LIMIT 8193')
    channels = cursor.fetchall()
    if len(channels) > MAX_RECORDS:
        raise ValueError('Too many channels for the native social cache')
    for row in channels:
        records['channels'].append((channel_key(row[0], row[1]), channel_document(row)))
    cursor.execute('SELECT guildid,name,leaderguid,motd,info FROM guild ORDER BY guildid LIMIT 8193')
    guilds = cursor.fetchall()
    if len(guilds) > MAX_RECORDS:
        raise ValueError('Too many guilds for the native social cache')
    cursor.execute('SELECT guildid,rid,rname,rights FROM guild_rank ORDER BY guildid,rid LIMIT 81921')
    ranks = {}
    for guild_id, *rank in cursor.fetchall():
        ranks.setdefault(guild_id, []).append(rank)
    cursor.execute('SELECT guildid,guid,member_rank,pnote,offnote FROM guild_member ORDER BY guildid,guid LIMIT 65537')
    members = {}
    unique_members = set()
    for guild_id, guid, *member in cursor.fetchall():
        if guid in unique_members:
            raise ValueError('A legacy character belongs to multiple guilds')
        unique_members.add(guid)
        members.setdefault(guild_id, []).append([guid, *member])
    if sum(len(value) for value in ranks.values()) > 81920 or len(unique_members) > 65536:
        raise ValueError('Legacy guild data exceeds import bounds')
    ids = {row[0] for row in guilds}
    if set(ranks) - ids or set(members) - ids:
        raise ValueError('Orphaned legacy guild ranks or members require repair before import')
    for row in guilds:
        records['guilds'].append(('guild-' + str(row[0]), guild_document(row, ranks.get(row[0], []), members.get(row[0], []))))
    for domain, entries in records.items():
        keys = [key for key, _ in entries]
        if len(keys) != len(set(keys)):
            raise ValueError('Legacy channel names collide after normalization')
        encoded = [(key, document(domain, value)) for key, value in entries]
        if sum(len(key.encode()) + len(value) for key, value in encoded) > MAX_BYTES:
            raise ValueError('Legacy social data exceeds the native per-domain cache limit')
        records[domain] = encoded
    return records


def stage(connection, realm, apply=False):
    """Caller owns the private DB connection; this function holds the service lock."""
    integer(realm, 0xffffffff, 1)
    acquired = False
    try:
        with connection.cursor() as cursor:
            cursor.execute('SELECT DATABASE()')
            schema = cursor.fetchone()[0]
            lock = 'skyfire-character:' + hashlib.sha256(schema.encode()).hexdigest()[:40]
            cursor.execute('SELECT GET_LOCK(%s,0)', (lock,))
            acquired = cursor.fetchone()[0] == 1
            if not acquired:
                raise RuntimeError('Stop the character service before staging legacy social data')
            placeholders = ','.join(['%s'] * (len(SOURCE_TABLES) + len(DESTINATION_TABLES)))
            cursor.execute('SELECT table_name,engine FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name IN (' + placeholders + ')', SOURCE_TABLES + DESTINATION_TABLES)
            engines = dict(cursor.fetchall())
            if set(engines) != set(SOURCE_TABLES + DESTINATION_TABLES) or any(value != 'InnoDB' for value in engines.values()):
                raise RuntimeError('Install the social schema and ensure source and destination tables use InnoDB')
            cursor.execute('SET TRANSACTION ISOLATION LEVEL SERIALIZABLE')
        connection.begin()
        with connection.cursor() as cursor:
            for table in DESTINATION_TABLES:
                cursor.execute('SELECT 1 FROM ' + table + ' WHERE realm=%s LIMIT 1 FOR UPDATE', (realm,))
                if cursor.fetchone():
                    raise RuntimeError('Social destination is not empty; refusing to overwrite existing ownership or state')
            records = load_records(cursor)
            if apply:
                instance = secrets.token_hex(16)
                for domain, entries in records.items():
                    cursor.execute('INSERT INTO character_social_owners(realm,domain,epoch,node,instance,revision) VALUES(%s,%s,1,%s,%s,%s)',
                                   (realm, domain, 'legacy-import', instance, len(entries)))
                    for revision, (key, value) in enumerate(entries, 1):
                        cursor.execute('INSERT INTO character_social_records(realm,domain,record_key,revision,document) VALUES(%s,%s,%s,%s,%s)',
                                       (realm, domain, key.encode(), revision, value))
                        cursor.execute('INSERT INTO character_social_outbox(realm,domain,revision,record_key,document,actor) VALUES(%s,%s,%s,%s,%s,0)',
                                       (realm, domain, revision, key.encode(), value))
        if apply:
            connection.commit()
        else:
            connection.rollback()
        return {domain: len(entries) for domain, entries in records.items()}
    except BaseException:
        connection.rollback()
        raise
    finally:
        if acquired:
            with connection.cursor() as cursor:
                cursor.execute('SELECT RELEASE_LOCK(%s)', (lock,))
                cursor.fetchone()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', default='characterserver.toml')
    parser.add_argument('--apply', action='store_true', help='Commit staging rows; default is validation with rollback')
    parser.add_argument('--offline', action='store_true', help='Confirm all worlds and social daemons using this database are stopped')
    args = parser.parse_args()
    if not args.offline:
        parser.error('--offline is required: stop all worlds, chat and character services first')
    config = tomllib.loads(Path(args.config).read_text(encoding='utf-8-sig'))
    import pymysql
    connection = pymysql.connect(host=config['mysql_host'], port=config.get('mysql_port', 3306),
                                 user=config['mysql_user'], password=config['mysql_password'],
                                 database=config['mysql_database'], charset='utf8mb4', autocommit=True,
                                 connect_timeout=5, read_timeout=30, write_timeout=30)
    try:
        counts = stage(connection, config['realm_id'], args.apply)
        print(json.dumps({'applied': args.apply, 'records': counts, 'ownership_activated': False}))
    finally:
        connection.close()


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        # Driver errors can contain source values or credentials; never print them.
        print('Social import failed (' + type(error).__name__ + '); no ownership was activated. Check schema and offline prerequisites.', file=sys.stderr)
        sys.exit(1)
