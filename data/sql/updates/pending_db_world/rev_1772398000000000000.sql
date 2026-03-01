-- Arcane Blast debuff: spell_proc override to consume at CAST phase via family masks (AM/AE/ABarr only)
UPDATE `spell_proc` SET `ProcFlags`=69632, `SpellFamilyMask0`=6144, `SpellFamilyMask1`=32768, `SpellFamilyMask2`=0, `SpellPhaseMask`=1, `Charges`=1 WHERE `SpellId`=36032;
