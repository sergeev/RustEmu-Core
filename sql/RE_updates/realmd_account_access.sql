-- realmd table update for warden
ALTER TABLE `account`
    ADD COLUMN `os` VARCHAR(4) DEFAULT '' NOT NULL AFTER `locale`;

DROP TABLE IF EXISTS `account_access`;
CREATE TABLE `account_access` (
  `id` int(10) unsigned NOT NULL,
  `gmlevel` tinyint(3) unsigned NOT NULL,
  `RealmID` int(11) NOT NULL DEFAULT '-1',
  PRIMARY KEY (`id`,`RealmID`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8 ROW_FORMAT=DYNAMIC COMMENT='Account Access System';

INSERT IGNORE INTO `account_access` (`id`,`gmlevel`,`RealmID`) 
    SELECT `id`, `gmlevel`, -1 FROM `account` 
    WHERE `account`.`gmlevel` > 0;

ALTER TABLE `account` DROP `gmlevel`;

ALTER TABLE realmd_db_version CHANGE COLUMN required_10008_01_realmd_realmd_db_version required_12857_01_realmd_account_access bit;

DROP TABLE IF EXISTS `multi_IP_whitelist`;
CREATE TABLE `multi_IP_whitelist` (
  `id` int(32) NOT NULL AUTO_INCREMENT,
  `whitelist` varchar(500) DEFAULT NULL,
  `comment` longtext,
  PRIMARY KEY (`id`)
) DEFAULT CHARSET=utf8;