contract C {
    uint immutable x;
    constructor() public {
        x = 3;
        C.selector.selector;
        C.selector;
    }

    function selector() external view returns(uint) { return x; }
}
// ----
// TypeError: (194-195): Immutable variables cannot be read during contract creation time, which means they cannot be read in the constructor or any function or modifier called from it.
