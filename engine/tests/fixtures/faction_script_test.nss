void main()
{
    object oTarget = GetObjectByTag("end_gensith");
    int nBefore = GetReputation(OBJECT_SELF, oTarget);
    AdjustReputation(OBJECT_SELF, oTarget, 25);
    int nAfter = GetReputation(OBJECT_SELF, oTarget);
    SetLocalNumber(OBJECT_SELF, 0, nBefore);
    SetLocalNumber(OBJECT_SELF, 1, nAfter);
}
