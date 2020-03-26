contract C {
    uint immutable x;
    constructor() public {
        x = 3;
        C.readX.selector;
    }

    function readX() external view returns(uint) { return x; }
}
// ----
// Warning: (85-101): Statement has no effect.
