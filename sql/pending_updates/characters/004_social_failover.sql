-- This file is part of Project SkyFire https://www.projectskyfire.org.
-- See LICENSE.md file for Copyright information
CREATE TABLE character_social_leases (
 realm INT UNSIGNED NOT NULL PRIMARY KEY,
 epoch BIGINT UNSIGNED NOT NULL,
 node VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
 instance CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
 expires DATETIME(6) NOT NULL
) ENGINE=InnoDB;
CREATE TABLE character_social_commands (
 realm INT UNSIGNED NOT NULL,
 domain VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
 node VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
 generation CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
 sequence BIGINT UNSIGNED NOT NULL,
 account INT UNSIGNED NOT NULL,
 actor BIGINT UNSIGNED NOT NULL,
 incarnation BIGINT UNSIGNED NOT NULL,
 guild INT UNSIGNED NOT NULL,
 committed TINYINT UNSIGNED NOT NULL,
 revision BIGINT UNSIGNED NULL,
 document MEDIUMBLOB NULL,
 PRIMARY KEY(realm,domain,node,generation,sequence)
) ENGINE=InnoDB;
