-- This file is part of Project SkyFire https://www.projectskyfire.org.
-- See LICENSE.md file for Copyright information
-- Prevent an in-flight world inventory save from resurrecting a consumed charter.
CREATE TABLE character_social_consumed_items (
 realm INT UNSIGNED NOT NULL,
 instance CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
 item INT UNSIGNED NOT NULL,
 PRIMARY KEY(realm,instance,item)
) ENGINE=InnoDB;
