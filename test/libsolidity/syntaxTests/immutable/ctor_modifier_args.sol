contract C {
    uint immutable x;
    constructor() readX(x = 3) public { }

    modifier readX(uint _x) {
        _; f(_x);
    }

    function f(uint a) internal pure {}
}
// ----
// TypeError: (59-60): Immutable variables must be initialized in the constructor body.
