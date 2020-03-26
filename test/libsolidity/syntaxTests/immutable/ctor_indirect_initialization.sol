contract C {
    uint immutable x;
    constructor() public {
        initX();
    }

    function initX() internal {
        x = 3;
    }
}
// ----
// TypeError: (126-127): Immutable variables can only be initialized inline or directly in the constructor.
