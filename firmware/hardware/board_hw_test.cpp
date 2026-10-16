#include "pch.h"
#include "board_overrides.h"
#include <array>

// Add this board's HW QC output pins and update the array size to match.
// For example: std::array<Gpio, 2> OUTPUTS = {Gpio::B5, Gpio::B6};
// The generic example has no assigned outputs.
static std::array<Gpio, 0> OUTPUTS = {
};

// release_20260812 API: these are weak symbols overridden directly. The custom_board_getMeta*
// hooks used by fw-custom-example master are missing or declared-only in this release.
int getBoardMetaOutputsCount() {
    return static_cast<int>(OUTPUTS.size());
}

Gpio* getBoardMetaOutputs() {
    return OUTPUTS.empty() ? nullptr : OUTPUTS.data();
}

int getBoardMetaDcOutputsCount() {
    // Set this to the number of DC motor outputs on the board.
    return 0;
}

void setupBoardHardwareTestOverrides() {
}
