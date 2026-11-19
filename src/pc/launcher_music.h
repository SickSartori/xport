#pragma once

// The launcher's background music (res/launcher_music.mp3, in the runtime DLL's resources): decoded with Media
// Foundation on a helper thread, looped on its own XAudio2 engine, faded in and out.

#include <memory>

namespace xport::pc {

class LauncherMusic {
public:
    LauncherMusic();
    ~LauncherMusic();  // fades out and stops
    // Call every frame: the fade follows `target` (0..1, times the volume setting).
    void update(float dt, float target);
    // How loud the music is right now, 0..1 (the fade and the volume setting left out): for pictures that move with it.
    float loudness() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace xport::pc
