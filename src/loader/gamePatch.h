#ifndef KYTY_LOADER_GAME_PATCH_H_
#define KYTY_LOADER_GAME_PATCH_H_

#include "common/common.h"

#include <filesystem>
#include <cstddef>
#include <string>
#include <vector>

namespace Loader {

struct Program;

namespace GamePatch {

// The menu reads immutable snapshots, never directly modifies guest addresses.
struct CheatInfo {
    std::string name;
    bool enabled = false;       // desired selection, also saved for next launch
    bool applied = false;       // currently active in the running game
    bool live = false;          // verified one-byte reversible patch
};
struct CheatToggleResult {
    bool ok = false;
    bool live = false;
    std::string message;
};
[[nodiscard]] std::vector<CheatInfo> ListCheats();
[[nodiscard]] std::string CheatStatus();
[[nodiscard]] CheatToggleResult ToggleCheat(size_t index);

bool Apply(const std::filesystem::path& plan_path, Program* main_program,
           const std::vector<Program*>& programs);
bool ApplyPending(Program* program);
void Clear();

} // namespace GamePatch

} // namespace Loader

#endif // KYTY_LOADER_GAME_PATCH_H_
