# This file is part of Project SkyFire https://www.projectskyfire.org.
# See LICENSE.md file for Copyright information
"""Guild lifecycle integration using the shipped character DDL in an isolated schema."""
import copy
import concurrent.futures
import threading
import json
from pathlib import Path
import re
import secrets
import time


def exercise(test, admin, config):
    import pymysql
    from database import CharacterDatabase
    from social_store import canonical
    from wire import blob, u32

    root = Path(__file__).resolve().parents[2]
    service = root / 'src/server/characterserver'
    schema = 'skyfire_social_lifecycle_' + secrets.token_hex(8)
    connection = pymysql.connect(host=config['mysql_host'], port=config.get('mysql_port', 3306),
                                 user=config['mysql_user'], password=config['mysql_password'], autocommit=True)
    database = None
    session = instance = None
    try:
        with connection.cursor() as cursor:
            cursor.execute('CREATE DATABASE `' + schema + '` CHARACTER SET utf8mb4')
            cursor.execute('USE `' + schema + '`')
            ddl = (root / 'sql/base/characters_database.sql').read_text()
            statements = re.findall(r'^CREATE TABLE `[^`]+` \(.*?\) ENGINE=[^;]+;', ddl, re.M | re.S)
            test.assertGreater(len(statements), 50)
            for statement in statements:
                cursor.execute(statement)
            migrations = [path.read_text() for folder in ('pending_updates', 'updates')
                          for path in (root / 'sql' / folder / 'characters').glob('*.sql')
                          if re.search(r'CREATE TABLE character_social_(owners|guild_members|consumed_items) \(', path.read_text())]
            test.assertEqual(len(migrations), 3)
            for migration in migrations:
                for statement in migration.split(';'):
                    if statement.strip():
                        cursor.execute(statement)
            cursor.execute("INSERT INTO characters(guid,realm,account,name,taximask) VALUES(100,1,10,'Founder',''),(200,1,20,'Signer','')")
            cursor.execute("INSERT INTO petition(ownerguid,petitionguid,name,type) VALUES(100,7001,'Lifecycle',4)")
            cursor.execute('INSERT INTO petition_sign(ownerguid,petitionguid,playerguid,player_account,type) VALUES(100,7001,200,20,4)')
            cursor.execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,enchantments) VALUES(7001,5863,100,'')")
            cursor.execute('INSERT INTO character_inventory(guid,bag,slot,item) VALUES(100,0,0,7001)')
        options = config | dict(mysql_database=schema, allowed_chat_nodes=['chat-lifecycle'], social_guild_projection=True)
        database = CharacterDatabase(options, service / 'statements.json')
        instance, world_epoch = database.attach('world-lifecycle', b'\2' + u32(1) + blob(secrets.token_hex(16)) + blob(database.catalog_hash))
        session, epoch = database.social.attach('chat-lifecycle', b'\1' + u32(1) + blob(secrets.token_hex(16)) + blob('guilds'))
        document = dict(id=9001, name='Lifecycle', leader=100, motd='Fixture', info='',
                        ranks=[dict(name='Rank' + str(rank), rights=255 if not rank else 64) for rank in range(5)],
                        members=[dict(guid=100, rank=0, public_note='', officer_note='secret'),
                                 dict(guid=200, rank=4, public_note='', officer_note='')])
        request = dict(id=secrets.token_hex(16), key='guild-9001', expected=0, actor=100, document=document,
                       context=dict(petition=7001, minimum_signatures=1, game_master=False))

        def mutate(value):
            return json.loads(database.social.execute('chat-lifecycle', session, epoch, b'\x12' + blob(canonical(value))))['revision']

        def scalar(sql, args=()):
            with connection.cursor() as cursor:
                cursor.execute(sql, args)
                return cursor.fetchone()[0]

        def typed(name, parameters):
            values = [b'\1' + blob(str(value)) if isinstance(value, int) else b'\2' + blob(value) for value in parameters]
            return b'\1' + u32(database.statement_ids[name]) + u32(len(values)) + b''.join(values)

        def world(statements):
            payload = b'\3' + blob(secrets.token_hex(16)) + u32(len(statements)) + b''.join(statements)
            return database.execute('world-lifecycle', instance, world_epoch, payload)

        # Startup instance cleanup joins characters without deleting characters.
        with connection.cursor() as cursor:
            cursor.execute('INSERT INTO character_instance(guid,instance) VALUES(100,1),(9999,2)')
        cleanup = 'DELETE ci.* FROM character_instance AS ci LEFT JOIN characters AS c ON ci.guid = c.guid WHERE c.guid IS NULL'
        world([b'\0' + blob(cleanup)])
        world([b'\0' + blob('DELETE FROM parties WHERE leaderGuid NOT IN (SELECT guid FROM characters)'),
               b'\0' + blob('DELETE FROM party_member WHERE memberGuid NOT IN (SELECT guid FROM characters)')])
        test.assertEqual(scalar('SELECT COUNT(*) FROM character_instance'), 1)
        test.assertEqual(scalar('SELECT COUNT(*) FROM characters'), 2)
        with test.assertRaises(ValueError):
            world([b'\0' + blob('DELETE FROM characters WHERE guid=100')])

        # A receipt failure rolls the charter, membership index and economy projection back together.
        with connection.cursor() as cursor:
            cursor.execute("CREATE TRIGGER fail_lifecycle_receipt BEFORE INSERT ON character_social_receipts FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='fixture rollback'")
        try:
            with test.assertRaises(Exception):
                mutate(request)
            for table in ('petition', 'petition_sign', 'item_instance', 'character_inventory'):
                test.assertEqual(scalar('SELECT COUNT(*) FROM ' + table), 1)
            for table in ('guild', 'guild_member', 'character_social_records', 'character_social_guild_members', 'character_social_consumed_items'):
                test.assertEqual(scalar('SELECT COUNT(*) FROM ' + table), 0)
        finally:
            with connection.cursor() as cursor:
                cursor.execute('DROP TRIGGER fail_lifecycle_receipt')
        # A separate world transaction holds its ownership row while a charter
        # mutation starts. The world must still acquire the social row without a
        # lock cycle; only after it releases ownership may creation finish.
        blocker = pymysql.connect(host=config['mysql_host'], port=config.get('mysql_port', 3306),
                                  user=config['mysql_user'], password=config['mysql_password'], database=schema,
                                  autocommit=False, read_timeout=5, write_timeout=5)
        attempted = threading.Event()
        first_lock = []
        original_cursor = database.db.cursor

        class ObservedCursor:
            def __init__(self, cursor):
                self.cursor = cursor

            def __enter__(self):
                self.cursor.__enter__()
                return self

            def __exit__(self, *args):
                return self.cursor.__exit__(*args)

            def __getattr__(self, name):
                return getattr(self.cursor, name)

            def execute(self, sql, args=None):
                if 'FOR UPDATE' in sql and ('character_service_owners' in sql or 'character_social_owners' in sql) and not first_lock:
                    first_lock.append(sql)
                    attempted.set()
                return self.cursor.execute(sql, args)

        try:
            with blocker.cursor() as cursor:
                cursor.execute('SET SESSION innodb_lock_wait_timeout=2')
                cursor.execute('SELECT realm FROM character_service_owners WHERE realm=1 FOR UPDATE')
                cursor.fetchall()
            database.db.cursor = lambda *args, **kwargs: ObservedCursor(original_cursor(*args, **kwargs))
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(mutate, request)
                try:
                    test.assertTrue(attempted.wait(3), 'Guild mutation did not reach its ownership lock')
                    test.assertIn('character_service_owners', first_lock[0])
                    with blocker.cursor() as cursor:
                        cursor.execute("SELECT revision FROM character_social_owners WHERE realm=1 AND domain='guilds' FOR UPDATE")
                        cursor.fetchall()
                    test.assertFalse(future.done(), 'Guild mutation bypassed the outstanding world owner transaction')
                finally:
                    blocker.rollback()
                revision = future.result(timeout=5)
        finally:
            database.db.cursor = original_cursor
            blocker.rollback()
            blocker.close()
        test.assertEqual(mutate(request), revision)
        test.assertEqual(scalar('SELECT COUNT(*) FROM guild_member'), 2)
        for table in ('petition', 'petition_sign', 'item_instance', 'character_inventory'):
            test.assertEqual(scalar('SELECT COUNT(*) FROM ' + table), 0)
        test.assertEqual(scalar('SELECT COUNT(*) FROM character_social_consumed_items'), 1)

        def receipt(request_id):
            return json.loads(database.social.execute('chat-lifecycle', session, epoch, b'\x14' + blob(canonical({'id': request_id}))))

        test.assertEqual(receipt(request['id']), {'found': True, 'revision': revision})
        test.assertEqual(receipt(secrets.token_hex(16)), {'found': False})
        with test.assertRaises(ValueError):
            receipt('not-a-receipt')
        with test.assertRaisesRegex(ValueError, 'already has an attached session'):
            database.social.attach('chat-lifecycle', b'\1' + u32(1) + blob(session[1]) + blob('guilds'))
        previous_instance = session[1]
        database.social.detach(session)
        session, epoch = database.social.attach('chat-lifecycle', b'\1' + u32(1) + blob(previous_instance) + blob('guilds'))
        test.assertEqual(receipt(request['id']), {'found': True, 'revision': revision})

        # A previously queued world save must not resurrect the consumed charter or its slot.
        world([typed('CHAR_REP_ITEM_INSTANCE', [5863, 100, 0, 0, 1, 0, '', 0, '', 0, 0, 0, 0, '', 7001]),
               typed('CHAR_REP_INVENTORY_ITEM', [100, 0, 0, 7001])])
        test.assertEqual(scalar('SELECT COUNT(*) FROM item_instance WHERE guid=7001'), 0)
        test.assertEqual(scalar('SELECT COUNT(*) FROM character_inventory WHERE item=7001'), 0)

        # Rank permissions and economic limits share the social receipt transaction.
        with connection.cursor() as cursor:
            cursor.execute('INSERT INTO guild_bank_tab(guildid,TabId) VALUES(9001,0),(9001,2)')
        ranked = copy.deepcopy(document)
        ranked['ranks'][2] = dict(name='Trusted', rights=64)
        bank = dict(rank=2, money=123456, tabs=[dict(rights=3+tab, slots=7+tab) for tab in range(8)])
        rank_request = dict(id=secrets.token_hex(16), key='guild-9001', expected=revision, actor=100,
                            document=ranked, context=dict(rank_bank=bank))
        with test.assertRaisesRegex(ValueError, 'bank rank proof'):
            mutate(rank_request | {'actor': 200})
        with test.assertRaisesRegex(ValueError, 'bank rank proof'):
            mutate(rank_request | {'context': dict(rank_bank=bank | {'tabs': bank['tabs'][:7]})})
        with connection.cursor() as cursor:
            cursor.execute("CREATE TRIGGER fail_rank_receipt BEFORE INSERT ON character_social_receipts FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='fixture rollback'")
        try:
            with test.assertRaises(Exception):
                mutate(rank_request)
            test.assertEqual(scalar('SELECT BankMoneyPerDay FROM guild_rank WHERE guildid=9001 AND rid=2'), 0)
            test.assertEqual(scalar('SELECT rname FROM guild_rank WHERE guildid=9001 AND rid=2'), 'Rank2')
            test.assertEqual(scalar('SELECT COUNT(*) FROM guild_bank_right WHERE guildid=9001'), 0)
            test.assertEqual(receipt(rank_request['id']), {'found': False})
        finally:
            with connection.cursor() as cursor:
                cursor.execute('DROP TRIGGER fail_rank_receipt')
        revision = mutate(rank_request)
        test.assertEqual(scalar('SELECT BankMoneyPerDay FROM guild_rank WHERE guildid=9001 AND rid=2'), 123456)
        test.assertEqual(scalar('SELECT rname FROM guild_rank WHERE guildid=9001 AND rid=2'), 'Trusted')
        with connection.cursor() as cursor:
            cursor.execute('SELECT TabId,gbright,SlotPerDay FROM guild_bank_right WHERE guildid=9001 AND rid=2 ORDER BY TabId')
            test.assertEqual(cursor.fetchall(), ((0, 3, 7), (2, 5, 9)))
        document = ranked
        revision = mutate(dict(id=secrets.token_hex(16), key='guild-9001', expected=revision, actor=100,
                               document=document, context=dict(rank_bank=dict(rank=0, money=1, tabs=[dict(rights=0, slots=1) for _ in range(8)]))))
        test.assertEqual(scalar('SELECT BankMoneyPerDay FROM guild_rank WHERE guildid=9001 AND rid=0'), 0xffffffff)
        with connection.cursor() as cursor:
            cursor.execute('SELECT gbright,SlotPerDay FROM guild_bank_right WHERE guildid=9001 AND rid=0 ORDER BY TabId')
            test.assertEqual(cursor.fetchall(), ((255, 0xffffffff), (255, 0xffffffff)))

        # Removing a middle rank preserves the surviving economic rights at their new indices.
        with connection.cursor() as cursor:
            for rank in range(5):
                cursor.execute('UPDATE guild_rank SET BankMoneyPerDay=%s WHERE guildid=9001 AND rid=%s', (100 + rank, rank))
                cursor.execute('REPLACE INTO guild_bank_right(guildid,TabId,rid,gbright,SlotPerDay) VALUES(9001,0,%s,1,%s)', (rank, 10 + rank))
        reduced = copy.deepcopy(document)
        del reduced['ranks'][2]
        reduced['members'][1]['rank'] = 3
        revision = mutate(dict(id=secrets.token_hex(16), key='guild-9001', expected=revision, actor=100,
                               document=reduced, context=dict(rank_removed=2)))
        with connection.cursor() as cursor:
            cursor.execute('SELECT rid,BankMoneyPerDay FROM guild_rank WHERE guildid=9001 ORDER BY rid')
            test.assertEqual(cursor.fetchall(), ((0, 100), (1, 101), (2, 103), (3, 104)))
            cursor.execute('SELECT rid,SlotPerDay FROM guild_bank_right WHERE guildid=9001 AND TabId=0 ORDER BY rid')
            test.assertEqual(cursor.fetchall(), ((0, 10), (1, 11), (2, 13), (3, 14)))

        # Character deletion elects one successor and advances the social outbox atomically.
        world([typed('CHAR_DEL_CHARACTER', [100])])
        test.assertEqual(scalar('SELECT COUNT(*) FROM characters WHERE guid=100'), 0)
        test.assertEqual(scalar('SELECT leaderguid FROM guild WHERE guildid=9001'), 200)
        stored = json.loads(scalar("SELECT document FROM character_social_records WHERE domain='guilds' AND record_key='guild-9001'"))
        test.assertEqual(stored['leader'], 200)
        test.assertEqual([(member['guid'], member['rank']) for member in stored['members']], [(200, 0)])
        revision = scalar("SELECT revision FROM character_social_records WHERE domain='guilds' AND record_key='guild-9001'")

        # Economic operations lock the current parent and rank before writing.
        world([typed('CHAR_UPD_GUILD_BANK_MONEY', [12345, 9001])])
        test.assertEqual(scalar('SELECT BankMoney FROM guild WHERE guildid=9001'), 12345)
        with test.assertRaisesRegex(ValueError, 'rank no longer exists'):
            world([typed('CHAR_INS_GUILD_BANK_RIGHT', [9001, 0, 99, 1, 1])])
        test.assertEqual(scalar('SELECT COUNT(*) FROM guild_bank_right WHERE rid=99'), 0)

        # Disband removes bank inventory and its auxiliary item state, ranks and all social membership.
        with connection.cursor() as cursor:
            cursor.execute("INSERT INTO item_instance(guid,itemEntry,enchantments) VALUES(7002,123,'')")
            cursor.execute('INSERT INTO guild_bank_item(guildid,TabId,SlotId,item_guid) VALUES(9001,0,0,7002)')
            cursor.execute('INSERT INTO item_refund_instance(item_guid,player_guid) VALUES(7002,200)')
            cursor.execute("INSERT INTO item_soulbound_trade_data(itemGuid,allowedPlayers) VALUES(7002,'200')")
            cursor.execute('INSERT INTO item_instance_transmog(itemGuid,transmogrifyId) VALUES(7002,555)')
            cursor.execute('INSERT INTO guild_member_withdraw(guid) VALUES(200)')
        mutate(dict(id=secrets.token_hex(16), key='guild-9001', expected=revision, actor=200, document=None))
        for table in ('guild', 'guild_rank', 'guild_member', 'guild_member_withdraw', 'guild_bank_item', 'guild_bank_right',
                      'item_instance', 'item_refund_instance', 'item_soulbound_trade_data', 'item_instance_transmog', 'character_social_guild_members'):
            test.assertEqual(scalar('SELECT COUNT(*) FROM ' + table), 0, table)
        test.assertIsNone(scalar("SELECT document FROM character_social_records WHERE domain='guilds' AND record_key='guild-9001'"))
        # A queued deposit must fail atomically after disband, without debiting
        # a character or recreating the deleted guild's auxiliary records.
        previous_money = scalar('SELECT money FROM characters WHERE guid=200')
        with test.assertRaisesRegex(ValueError, 'target no longer exists'):
            world([typed('CHAR_UDP_CHAR_MONEY', [42, 200]), typed('CHAR_INS_GUILD_BANK_TAB', [9001, 0])])
        test.assertEqual(scalar('SELECT money FROM characters WHERE guid=200'), previous_money)
        test.assertEqual(scalar('SELECT COUNT(*) FROM guild_bank_tab WHERE guildid=9001'), 0)
        # Console capabilities permit administrative creation without a player actor,
        # while malformed console claims still fail before any projection changes.
        with connection.cursor() as cursor:
            cursor.execute("INSERT INTO characters(guid,realm,account,name,taximask) VALUES(300,1,30,'GMFounder',''),(400,1,40,'Successor',''),(500,1,50,'OtherOwner','')")
        gm = dict(id=9002, name='GMLifecycle', leader=300, motd='', info='',
                  ranks=[dict(name='Rank'+str(rank), rights=255 if not rank else 64) for rank in range(5)],
                  members=[dict(guid=300, rank=0, public_note='', officer_note='')])
        gm_context = dict(petition=0, minimum_signatures=0, game_master=True, admin_permission=402, console=True, founder=300)
        gm_request = dict(id=secrets.token_hex(16), key='guild-9002', expected=0, actor=0, document=gm, context=gm_context)
        with test.assertRaises(ValueError):
            mutate(gm_request | {'context': gm_context | {'console': False}})
        with test.assertRaises(ValueError):
            mutate(gm_request | {'context': gm_context | {'admin_permission': 401}})
        gm_revision = mutate(gm_request)
        test.assertEqual(scalar('SELECT leaderguid FROM guild WHERE guildid=9002'), 300)
        gm['name'] = 'GMLifecycleRenamed'
        gm_revision = mutate(dict(id=secrets.token_hex(16), key='guild-9002', expected=gm_revision, actor=0,
                                  document=gm, context=dict(admin_permission=407, console=True)))
        test.assertEqual(scalar('SELECT name FROM guild WHERE guildid=9002'), 'GMLifecycleRenamed')
        other = copy.deepcopy(gm)
        other.update(id=9003, name='OtherGuild', leader=500)
        other['members'] = [dict(guid=500, rank=0, public_note='', officer_note='')]
        mutate(dict(id=secrets.token_hex(16), key='guild-9003', expected=0, actor=0, document=other,
                    context=gm_context | {'founder': 500}))
        with test.assertRaisesRegex(ValueError, 'name already exists'):
            mutate(dict(id=secrets.token_hex(16), key='guild-9002', expected=gm_revision, actor=0,
                        document=gm | {'name': 'OtherGuild'}, context=dict(admin_permission=407, console=True)))
        test.assertEqual(scalar('SELECT name FROM guild WHERE guildid=9002'), 'GMLifecycleRenamed')
        gm['members'].append(dict(guid=400, rank=1, public_note='', officer_note=''))
        gm_revision = mutate(dict(id=secrets.token_hex(16), key='guild-9002', expected=gm_revision, actor=0,
                                  document=gm, context=dict(admin_permission=404, console=True)))
        claimed = copy.deepcopy(gm)
        claimed['leader'] = 400
        claimed['members'][0]['rank'] = 1
        claimed['members'][1]['rank'] = 0
        claim = dict(id=secrets.token_hex(16), key='guild-9002', expected=gm_revision, actor=400,
                     document=claimed, context=dict(claim_leader=300))
        with connection.cursor() as cursor:
            cursor.execute('UPDATE characters SET logout_time=%s,online=0 WHERE guid=300', (int(time.time()),))
        with test.assertRaisesRegex(ValueError, 'remains active'):
            mutate(claim)
        with connection.cursor() as cursor:
            cursor.execute('UPDATE characters SET logout_time=%s,online=1 WHERE guid=300', (int(time.time())-91*24*60*60,))
        with test.assertRaisesRegex(ValueError, 'remains active'):
            mutate(claim)
        with connection.cursor() as cursor:
            cursor.execute('UPDATE characters SET online=0 WHERE guid=300')
        with test.assertRaisesRegex(ValueError, 'Invalid leadership claim'):
            mutate(claim | {'context': dict(claim_leader=999)})
        gm_revision = mutate(claim)
        test.assertEqual(scalar('SELECT leaderguid FROM guild WHERE guildid=9002'), 400)

        # Faction-change removal must share the same typed transaction and character.
        with test.assertRaisesRegex(ValueError, 'same character faction-change'):
            world([typed('CHAR_DEL_GUILD_MEMBER', [400])])
        with test.assertRaisesRegex(ValueError, 'same character faction-change'):
            world([typed('CHAR_UPD_FACTION_OR_RACE', ['WrongTarget', 2, 0, 300]), typed('CHAR_DEL_GUILD_MEMBER', [400])])
        test.assertEqual(scalar('SELECT name FROM characters WHERE guid=300'), 'GMFounder')
        test.assertEqual(scalar('SELECT COUNT(*) FROM guild_member WHERE guid=400'), 1)
        faction = [typed('CHAR_UPD_FACTION_OR_RACE', ['Successor', 2, 0, 400]), typed('CHAR_DEL_GUILD_MEMBER', [400])]
        with connection.cursor() as cursor:
            cursor.execute("CREATE TRIGGER fail_faction_receipt BEFORE INSERT ON character_service_commits FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='fixture rollback'")
        try:
            with test.assertRaises(Exception):
                world(faction)
            test.assertEqual(scalar('SELECT race FROM characters WHERE guid=400'), 0)
            test.assertEqual(scalar('SELECT COUNT(*) FROM guild_member WHERE guid=400'), 1)
            test.assertEqual(scalar('SELECT leaderguid FROM guild WHERE guildid=9002'), 400)
        finally:
            with connection.cursor() as cursor:
                cursor.execute('DROP TRIGGER fail_faction_receipt')
        world(faction)
        test.assertEqual(scalar('SELECT race FROM characters WHERE guid=400'), 2)
        test.assertEqual(scalar('SELECT COUNT(*) FROM guild_member WHERE guid=400'), 0)
        test.assertEqual(scalar('SELECT COUNT(*) FROM character_social_guild_members WHERE guid=400'), 0)
        test.assertEqual(scalar('SELECT leaderguid FROM guild WHERE guildid=9002'), 300)
        gm_revision = scalar("SELECT revision FROM character_social_records WHERE domain='guilds' AND record_key='guild-9002'")
        mutate(dict(id=secrets.token_hex(16), key='guild-9002', expected=gm_revision, actor=0, document=None,
                    context=dict(admin_permission=403, console=True)))
        test.assertEqual(scalar('SELECT COUNT(*) FROM guild WHERE guildid=9002'), 0)
        database.social.detach(session)
        session, epoch = database.social.attach('chat-lifecycle', b'\1' + u32(1) + blob(secrets.token_hex(16)) + blob('guilds'))
        test.assertEqual(receipt(request['id']), {'found': False})  # Earlier incarnation is not proof for this owner.
    finally:
        if database is not None:
            if session is not None:
                database.social.detach(session)
            if instance is not None:
                database.detach(instance)
            database.close()
        if not re.fullmatch(r'skyfire_social_lifecycle_[0-9a-f]{16}', schema):
            raise RuntimeError('Refusing to drop an unexpected fixture database')
        with connection.cursor() as cursor:
            cursor.execute('DROP DATABASE IF EXISTS `' + schema + '`')
        connection.close()
