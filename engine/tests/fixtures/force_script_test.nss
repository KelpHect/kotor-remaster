void main()
{
    object self = OBJECT_SELF;
    SetGoodEvilValue(self, 77);
    SetLocalNumber(self, 0, GetGoodEvilValue(self));
    SetLocalNumber(self, 1, GetHasFeat(28, self));
    SetLocalNumber(self, 2, GetHasSpell(35, self));
    SetLocalNumber(self, 3, GetCurrentForcePoints(self));
    SetLocalNumber(self, 4, GetMaxForcePoints(self));
    SetLocalNumber(self, 5, GetTypeFromTalent(TalentSpell(35)));
    SetLocalNumber(self, 6, GetIdFromTalent(TalentSpell(35)));
    ActionUseFeat(28, self);
    ActionCastSpellAtObject(17, self);
    ActionUseTalentOnObject(TalentSpell(17), self);
}
