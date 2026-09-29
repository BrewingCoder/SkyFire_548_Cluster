# This file is part of Project SkyFire https://www.projectskyfire.org.
# See LICENSE.md file for Copyright information
"""Guild social/economic projection changes, inside the caller's receipt transaction."""
import time


def one(cursor):
    row = cursor.fetchone()
    cursor.fetchall()
    return row


def cleanup_guild(cursor, guild):
    cursor.execute('SELECT item_guid FROM guild_bank_item WHERE guildid=%s', (guild,))
    for (item,) in cursor.fetchall():
        cursor.execute('DELETE FROM item_refund_instance WHERE item_guid=%s', (item,))
        cursor.execute('DELETE FROM item_soulbound_trade_data WHERE itemGuid=%s', (item,))
        cursor.execute('DELETE FROM item_instance_transmog WHERE itemGuid=%s', (item,))
        cursor.execute('DELETE FROM item_instance WHERE guid=%s', (item,))
    cursor.execute('SELECT guid FROM guild_member WHERE guildid=%s', (guild,))
    for (guid,) in cursor.fetchall():
        cursor.execute('DELETE FROM guild_member_withdraw WHERE guid=%s', (guid,))
    for table in ('guild_bank_item','guild_bank_tab','guild_bank_right','guild_bank_eventlog','guild_eventlog',
                  'guild_finder_applicant','guild_finder_guild_settings','guild_achievement',
                  'guild_achievement_progress','guild_newslog','guild_member','guild_rank','guild'):
        cursor.execute('DELETE FROM '+table+' WHERE guildid=%s', (guild,))


def create_guild(cursor, value, actor, context, realm):
    admin = context.get('admin_permission')
    required = {'petition','minimum_signatures','game_master'} | ({'admin_permission','console','founder'} if admin is not None else set())
    if set(context) != required:
        raise ValueError('Guild creation proof required')
    petition, minimum, gm = context['petition'], context['minimum_signatures'], context['game_master']
    if type(petition) is not int or not 0 <= petition <= 0xffffffff or type(minimum) is not int or not 0 <= minimum <= 9 or type(gm) is not bool:
        raise ValueError('Invalid guild creation proof')
    members = {row['guid'] for row in value['members']}
    signature_accounts = {}
    founder = context.get('founder',actor)
    if founder != value['leader'] or len(value['ranks']) != 5 or (admin is not None and admin != 402):
        raise ValueError('Invalid guild founder')
    cursor.execute('SELECT guildid FROM guild WHERE name=%s LIMIT 1 FOR UPDATE', (value['name'],))
    if one(cursor):
        raise ValueError('Guild name already exists')
    if gm:
        # This capability originates in the authenticated world's GM adapter;
        # the player-facing protocol cannot supply it directly.
        if petition or minimum or members != {founder} or admin != 402:
            raise ValueError('Invalid administrative creation')
    else:
        cursor.execute('SELECT ownerguid,name,type FROM petition WHERE petitionguid=%s FOR UPDATE', (petition,))
        if one(cursor) != (actor,value['name'],4):
            raise ValueError('Petition ownership mismatch')
        cursor.execute('SELECT playerguid,player_account FROM petition_sign WHERE petitionguid=%s AND type=4 FOR UPDATE', (petition,))
        signatures = cursor.fetchall()
        signature_accounts = dict(signatures)
        if len(signatures) < minimum or len(signatures) > 9 or len({row[1] for row in signatures}) != len(signatures) or members != ({row[0] for row in signatures} | {actor}):
            raise ValueError('Petition signatures changed')
        cursor.execute('SELECT owner_guid,itemEntry FROM item_instance WHERE guid=%s FOR UPDATE', (petition,))
        if one(cursor) != (actor,5863):
            raise ValueError('Guild charter item mismatch')
        cursor.execute('SELECT guid FROM character_inventory WHERE item=%s FOR UPDATE', (petition,))
        if one(cursor) != (actor,):
            raise ValueError('Guild charter is not in founder inventory')
        cursor.execute('SELECT instance FROM character_service_owners WHERE realm=%s FOR UPDATE', (realm,))
        world = one(cursor)
        if not world:
            raise ValueError('Guild creation requires an active world owner')
        cursor.execute('INSERT INTO character_social_consumed_items(realm,instance,item) VALUES(%s,%s,%s)', (realm,world[0],petition))
        cursor.execute('DELETE FROM character_inventory WHERE item=%s AND guid=%s', (petition,actor))
        cursor.execute('DELETE FROM item_instance WHERE guid=%s AND owner_guid=%s', (petition,actor))
        cursor.execute('DELETE FROM petition_sign WHERE petitionguid=%s', (petition,))
        cursor.execute('DELETE FROM petition WHERE petitionguid=%s', (petition,))
    for guid in members:
        cursor.execute('SELECT account FROM characters WHERE guid=%s FOR UPDATE', (guid,))
        character = one(cursor)
        if not character or not character[0]:
            raise ValueError('Guild founder/signatory character missing')
        if guid in signature_accounts and signature_accounts[guid] != character[0]:
            raise ValueError('Guild signatory account changed')
        if guid == founder and character[0] in signature_accounts.values():
            raise ValueError('Guild founder cannot sign their own petition')
        cursor.execute('SELECT guildid FROM guild_member WHERE guid=%s', (guid,))
        if one(cursor):
            raise ValueError('Guild founder/signatory already joined a guild')
    cursor.execute('INSERT INTO guild(guildid,name,leaderguid,motd,info,createdate) VALUES(%s,%s,%s,%s,%s,%s)',
                   (value['id'],value['name'],founder,value['motd'],value['info'],int(time.time())))
    for rank,row in enumerate(value['ranks']):
        cursor.execute('INSERT INTO guild_rank(guildid,rid,rname,rights,BankMoneyPerDay) VALUES(%s,%s,%s,%s,%s)',
                       (value['id'],rank,row['name'],row['rights'],0xffffffff if rank==0 else 0))


def project_guild(cursor, key, value, actor, context=None, previous=None, realm=1):
    guild = int(key.removeprefix('guild-'))
    context = dict(context or {})
    if 'admin_permission' in context:
        if type(context['admin_permission']) is not int or not 402 <= context['admin_permission'] <= 407 or type(context.get('console')) is not bool or (context['console'] != (actor==0)):
            raise ValueError('Invalid guild administrative capability')
        if context['admin_permission'] != 402:
            context.pop('admin_permission'); context.pop('console')
    elif not actor:
        raise ValueError('Guild player actor required')
    if 'claim_leader' in context:
        if set(context) != {'claim_leader'} or previous is None or context['claim_leader'] != previous['leader'] or value is None or value['leader'] != actor:
            raise ValueError('Invalid leadership claim')
        cursor.execute('SELECT logout_time,online FROM characters WHERE guid=%s FOR UPDATE', (context['claim_leader'],))
        leader = one(cursor)
        if not leader or leader[1] or leader[0] > int(time.time())-90*24*60*60:
            raise ValueError('Guild leader remains active')
        context = {}
    if value is None:
        if context:
            raise ValueError('Unexpected guild deletion context')
        cleanup_guild(cursor,guild)
        return
    cursor.execute('SELECT guildid FROM guild WHERE guildid=%s FOR UPDATE', (guild,))
    exists = one(cursor)
    if not exists:
        if previous is not None:
            raise ValueError('Guild projection unexpectedly missing')
        create_guild(cursor,value,actor,context or {},realm)
    else:
        cursor.execute('SELECT guildid FROM guild WHERE name=%s AND guildid<>%s LIMIT 1 FOR UPDATE', (value['name'],guild))
        if one(cursor):
            raise ValueError('Guild name already exists')
        cursor.execute('SELECT rid FROM guild_rank WHERE guildid=%s ORDER BY rid', (guild,))
        ranks = [row[0] for row in cursor.fetchall()]
        if ranks != list(range(len(ranks))):
            raise ValueError('Invalid legacy guild rank topology')
        count = len(value['ranks'])
        if count == len(ranks)+1:
            if context:
                raise ValueError('Unexpected rank append context')
            cursor.execute('INSERT INTO guild_rank(guildid,rid,rname,rights,BankMoneyPerDay) VALUES(%s,%s,%s,%s,0)',
                           (guild,count-1,value['ranks'][-1]['name'],value['ranks'][-1]['rights']))
        elif count == len(ranks)-1:
            if not context or set(context) != {'rank_removed'} or type(context['rank_removed']) is not int or not 0 < context['rank_removed'] < len(ranks):
                raise ValueError('Rank removal proof required')
            removed = context['rank_removed']
            cursor.execute('DELETE FROM guild_bank_right WHERE guildid=%s AND rid=%s', (guild,removed))
            cursor.execute('DELETE FROM guild_rank WHERE guildid=%s AND rid=%s', (guild,removed))
            for old in range(removed+1,len(ranks)):
                cursor.execute('UPDATE guild_rank SET rid=%s WHERE guildid=%s AND rid=%s', (old-1,guild,old))
                cursor.execute('UPDATE guild_bank_right SET rid=%s WHERE guildid=%s AND rid=%s', (old-1,guild,old))
        elif count == len(ranks) and set(context) == {'rank_bank'}:
            bank = context['rank_bank']
            if (previous is None or actor != previous['leader'] or type(bank) is not dict
                    or set(bank) != {'rank','money','tabs'} or type(bank['rank']) is not int
                    or not 0 <= bank['rank'] < count or type(bank['money']) is not int
                    or not 0 <= bank['money'] <= 0xffffffff or type(bank['tabs']) is not list
                    or len(bank['tabs']) != 8):
                raise ValueError('Invalid guild bank rank proof')
            for tab in bank['tabs']:
                if (type(tab) is not dict or set(tab) != {'rights','slots'}
                        or type(tab['rights']) is not int or not 0 <= tab['rights'] <= 255
                        or type(tab['slots']) is not int or not 0 <= tab['slots'] <= 0xffffffff):
                    raise ValueError('Invalid guild bank rank limits')
            rank = bank['rank']
            cursor.execute('UPDATE guild_rank SET BankMoneyPerDay=%s WHERE guildid=%s AND rid=%s',
                           (bank['money'] if rank else 0xffffffff,guild,rank))
            cursor.execute('SELECT TabId FROM guild_bank_tab WHERE guildid=%s', (guild,))
            purchased = [row[0] for row in cursor.fetchall()]
            for tab in purchased:
                if not 0 <= tab < 8:
                    raise ValueError('Invalid purchased guild bank tab')
                limits = bank['tabs'][tab]
                cursor.execute('REPLACE INTO guild_bank_right(guildid,TabId,rid,gbright,SlotPerDay) VALUES(%s,%s,%s,%s,%s)',
                               (guild,tab,rank,limits['rights'] if rank else 255,limits['slots'] if rank else 0xffffffff))
        elif count != len(ranks) or context:
            raise ValueError('Invalid guild topology context')
    cursor.execute('UPDATE guild SET name=%s,leaderguid=%s,motd=%s,info=%s WHERE guildid=%s',
                   (value['name'],value['leader'],value['motd'],value['info'],guild))
    for rank,row in enumerate(value['ranks']):
        cursor.execute('UPDATE guild_rank SET rname=%s,rights=%s WHERE guildid=%s AND rid=%s', (row['name'],row['rights'],guild,rank))
    cursor.execute('SELECT guid FROM guild_member WHERE guildid=%s', (guild,))
    old_members = {row[0] for row in cursor.fetchall()}
    for guid in old_members-{row['guid'] for row in value['members']}:
        cursor.execute('DELETE FROM guild_member WHERE guildid=%s AND guid=%s', (guild,guid))
        cursor.execute('DELETE FROM guild_member_withdraw WHERE guid=%s', (guid,))
    for row in value['members']:
        cursor.execute('SELECT guildid FROM guild_member WHERE guid=%s', (row['guid'],))
        existing = one(cursor)
        if existing and existing[0] != guild:
            raise ValueError('Legacy projection has conflicting guild membership')
        if existing:
            cursor.execute('UPDATE guild_member SET member_rank=%s,pnote=%s,offnote=%s WHERE guildid=%s AND guid=%s',
                           (row['rank'],row['public_note'],row['officer_note'],guild,row['guid']))
        else:
            cursor.execute('INSERT INTO guild_member(guildid,guid,member_rank,pnote,offnote) VALUES(%s,%s,%s,%s,%s)',
                           (guild,row['guid'],row['rank'],row['public_note'],row['officer_note']))
            cursor.execute('DELETE FROM petition_sign WHERE playerguid=%s AND type=4', (row['guid'],))
            cursor.execute('DELETE FROM petition_sign WHERE ownerguid=%s AND type=4', (row['guid'],))
            cursor.execute('DELETE FROM petition WHERE ownerguid=%s AND type=4', (row['guid'],))
