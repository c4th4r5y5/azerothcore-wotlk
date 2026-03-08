-- Remove SPELL_ATTR0_CU_DONT_BREAK_STEALTH (0x40) from Piercing Howl
UPDATE `spell_custom_attr` SET `attributes` = `attributes` & ~(64) WHERE `spell_id` = 12323;
