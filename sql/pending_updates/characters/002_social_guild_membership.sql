-- This file is part of Project SkyFire https://www.projectskyfire.org.
-- See LICENSE.md file for Copyright information
-- Rebuilt and validated from durable records when the guild authority attaches.
CREATE TABLE character_social_guild_members (
 realm INT UNSIGNED NOT NULL,
 guid BIGINT UNSIGNED NOT NULL,
 record_key VARBINARY(192) NOT NULL,
 PRIMARY KEY(realm,guid),
 KEY guild_record(realm,record_key)
) ENGINE=InnoDB;
