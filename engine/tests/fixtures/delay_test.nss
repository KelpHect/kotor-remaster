void main()
{
    object self = OBJECT_SELF;
    SetLocalNumber(self, 0, 1);
    DelayCommand(2.0, SetLocalNumber(self, 1, 42));
    AssignCommand(self, SetLocalNumber(self, 2, 7));
    object carth = GetObjectByTag("Carth");
    AssignCommand(carth, SetLocalNumber(OBJECT_SELF, 3, 9));
    DelayCommand(1.0, DelayCommand(1.0, SetLocalNumber(self, 4, 99)));
    SetLocalNumber(self, 5, 1);
    ActionWait(1.0);
    SetLocalNumber(self, 6, 2);
}
