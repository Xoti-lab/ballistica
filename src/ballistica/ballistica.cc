// Copyright (c) 2011-2020 Eric Froemling

#include "ballistica/ballistica.h"

#include <map>

#include "ballistica/app/app.h"
#include "ballistica/app/app_config.h"
#include "ballistica/app/app_globals.h"
#include "ballistica/audio/audio.h"
#include "ballistica/audio/audio_server.h"
#include "ballistica/core/fatal_error.h"
#include "ballistica/core/logging.h"
#include "ballistica/core/thread.h"
#include "ballistica/dynamics/bg/bg_dynamics.h"
#include "ballistica/dynamics/bg/bg_dynamics_server.h"
#include "ballistica/game/account.h"
#include "ballistica/graphics/graphics.h"
#include "ballistica/graphics/graphics_server.h"
#include "ballistica/input/input.h"
#include "ballistica/media/media.h"
#include "ballistica/media/media_server.h"
#include "ballistica/networking/network_write_module.h"
#include "ballistica/networking/networking.h"
#include "ballistica/platform/platform.h"
#include "ballistica/python/python.h"
#include "ballistica/scene/scene.h"

namespace ballistica {

// These are set automatically via script; don't change here.
const int kAppBuildNumber = 20194;
const char* kAppVersion = "1.5.26";
const char* kBlessingHash = nullptr;

// Our standalone globals.
// These are separated out for easy access.
// Everything else should go into AppGlobals (or more ideally into a class).
int g_early_log_writes{10};
Thread* g_main_thread{};
AppGlobals* g_app_globals{};
AppConfig* g_app_config{};
App* g_app{};
Account* g_account{};
Game* g_game{};
BGDynamics* g_bg_dynamics{};
BGDynamicsServer* g_bg_dynamics_server{};
Platform* g_platform{};
Utils* g_utils{};
UI* g_ui{};
Graphics* g_graphics{};
Python* g_python{};
Input* g_input{};
GraphicsServer* g_graphics_server{};
Media* g_media{};
Audio* g_audio{};
MediaServer* g_media_server{};
AudioServer* g_audio_server{};
StdInputModule* g_std_input_module{};
NetworkReader* g_network_reader{};
Networking* g_networking{};
NetworkWriteModule* g_network_write_module{};
TextGraphics* g_text_graphics{};

// Basic overview of our bootstrapping process:
// 1: All threads and globals are created and provisioned. Everything above
//    should exist at the end of this step (if it is going to exist).
//    Threads should not be talking to each other yet at this point.
// 2: The system is set in motion. Game thread is told to load/apply the config.
//    This kicks off an initial-screen-creation message sent to the
//    graphics-server thread. Other systems are informed that bootstrapping
//    is complete and they are free to talk to each other. Initial input-devices
//    are added, media loads can begin (at least ones not dependent on the
//    screen/renderer), etc.
// 3: The initial screen is created on the graphics-server thread in response
//    to the message sent from the game thread. A completion notice is sent
//    back to the game thread when done.
// 4: Back on the game thread, any renderer-dependent media-loads/etc. can begin
//    and lastly the initial game session is kicked off.

auto BallisticaMain(int argc, char** argv) -> int {
  try {
    // Even at the absolute start of execution we should be able to
    // phone home on errors. Set BA_CRASH_TEST=1 to test this.
    if (const char* crashenv = getenv("BA_CRASH_TEST")) {
      if (!strcmp(crashenv, "1")) {
        FatalError("Fatal-Error-Test");
      }
    }

    // -------------------------------------------------------------------------
    // Phase 1: Create and provision all globals.
    // -------------------------------------------------------------------------

    g_app_globals = new AppGlobals(argc, argv);
    g_platform = Platform::Create();
    g_platform->PostInit();
    g_account = new Account();
    g_utils = new Utils();
    Scene::Init();

    // Create a Thread wrapper around the current (main) thread.
    g_main_thread = new Thread(ThreadIdentifier::kMain, ThreadType::kMain);

    // Spin up g_app.
    g_platform->CreateApp();

    // Spin up our other standard threads.
    auto* media_thread = new Thread(ThreadIdentifier::kMedia);
    g_app_globals->pausable_threads.push_back(media_thread);
    auto* audio_thread = new Thread(ThreadIdentifier::kAudio);
    g_app_globals->pausable_threads.push_back(audio_thread);
    auto* game_thread = new Thread(ThreadIdentifier::kGame);
    g_app_globals->pausable_threads.push_back(game_thread);
    auto* network_write_thread = new Thread(ThreadIdentifier::kNetworkWrite);
    g_app_globals->pausable_threads.push_back(network_write_thread);

    // And add our other standard modules to them.
    game_thread->AddModule<Game>();
    network_write_thread->AddModule<NetworkWriteModule>();
    media_thread->AddModule<MediaServer>();
    g_main_thread->AddModule<GraphicsServer>();
    audio_thread->AddModule<AudioServer>();

    // Now let the platform spin up any other threads/modules it uses.
    // (bg-dynamics in non-headless builds, stdin/stdout where applicable, etc.)
    g_platform->CreateAuxiliaryModules();

    // Ok at this point we can be considered up-and-running.
    g_app_globals->is_bootstrapped = true;

    // -------------------------------------------------------------------------
    // Phase 2: Set things in motion.
    // -------------------------------------------------------------------------

    // Ok; now that we're bootstrapped, tell the game thread to read and apply
    // the config which should kick off the real action.
    g_game->PushApplyConfigCall();

    // Let the app and platform do whatever else it wants here such as adding
    // initial input devices/etc.
    g_app->OnBootstrapComplete();
    g_platform->OnBootstrapComplete();

    // -------------------------------------------------------------------------
    // Phase 3/4: Create a screen and/or kick off game (in other threads).
    // -------------------------------------------------------------------------

    if (g_app->UsesEventLoop()) {
      // On our event-loop using platforms we now simply sit in our event loop
      // until the app is quit.
      g_main_thread->RunEventLoop(false);
    } else {
      // In this case we'll now simply return and let the OS feed us events
      // until the app quits.
      // However we may need to 'prime the pump' first. For instance,
      // if the main thread event loop is driven by frame draws, it may need to
      // manually pump events until drawing begins (otherwise it will never
      // process the 'create-screen' event and wind up deadlocked).
      g_app->PrimeEventPump();
    }
  } catch (const std::exception& exc) {
    std::string error_msg =
        std::string("Unhandled exception in BallisticaMain(): ") + exc.what();

    FatalError::ReportFatalError(error_msg, true);
    bool exit_cleanly = !IsUnmodifiedBlessedBuild();
    bool handled = FatalError::HandleFatalError(exit_cleanly, true);

    // Do the default thing if it's not been handled.
    if (!handled) {
      if (exit_cleanly) {
        exit(1);
      } else {
        throw;
      }
    }
  }
  // printf("BLESSED? %d\n", static_cast<int>(IsUnmodifiedBlessedBuild()));

  g_platform->WillExitMain(false);
  return g_app_globals->return_value;
}

auto GetRealTime() -> millisecs_t {
  millisecs_t t = g_platform->GetTicks();

  // If we're at a different time than our last query, do our funky math.
  if (t != g_app_globals->last_real_time_ticks) {
    std::lock_guard<std::mutex> lock(g_app_globals->real_time_mutex);
    millisecs_t passed = t - g_app_globals->last_real_time_ticks;

    // GetTicks() is supposed to be monotonic but I've seen 'passed'
    // equal -1 even when it is using std::chrono::steady_clock. Let's do
    // our own filtering here to make 100% sure we don't go backwards.
    if (passed < 0) {
      passed = 0;
    } else {
      // Super big times-passed probably means we went to sleep or something;
      // clamp to a reasonable value.
      if (passed > 250) {
        passed = 250;
      }
    }
    g_app_globals->real_time += passed;
    g_app_globals->last_real_time_ticks = t;
  }
  return g_app_globals->real_time;
}

auto FatalError(const std::string& message) -> void {
  FatalError::ReportFatalError(message, false);
  bool exit_cleanly = !IsUnmodifiedBlessedBuild();
  bool handled = FatalError::HandleFatalError(exit_cleanly, false);
  assert(handled);
}

auto GetUniqueSessionIdentifier() -> const std::string& {
  static std::string session_id;
  static bool have_session_id = false;
  if (!have_session_id) {
    srand(static_cast<unsigned int>(
        Platform::GetCurrentMilliseconds()));       // NOLINT
    uint32_t tval = static_cast<uint32_t>(rand());  // NOLINT
    assert(g_platform);
    session_id = g_platform->GetUniqueDeviceIdentifier() + std::to_string(tval);
    have_session_id = true;
    if (session_id.size() >= 100) {
      Log("WARNING: session id longer than it should be.");
    }
  }
  return session_id;
}

auto InGameThread() -> bool {
  return (g_game && g_game->thread()->IsCurrent());
}

auto InMainThread() -> bool {
  return (g_app_globals
          && std::this_thread::get_id() == g_app_globals->main_thread_id);
}

auto InGraphicsThread() -> bool {
  return (g_graphics_server && g_graphics_server->thread()->IsCurrent());
}

auto InAudioThread() -> bool {
  return (g_audio_server && g_audio_server->thread()->IsCurrent());
}

auto InBGDynamicsThread() -> bool {
#if !BA_HEADLESS_BUILD
  return (g_bg_dynamics_server && g_bg_dynamics_server->thread()->IsCurrent());
#else
  return false;
#endif
}

auto InMediaThread() -> bool {
  return (g_media_server && g_media_server->thread()->IsCurrent());
}

auto InNetworkWriteThread() -> bool {
  return (g_network_write_module
          && g_network_write_module->thread()->IsCurrent());
}

auto GetInterfaceType() -> UIScale { return g_app_globals->ui_scale; }

void Log(const std::string& msg, bool to_stdout, bool to_server) {
  Logging::Log(msg, to_stdout, to_server);
}

auto IsVRMode() -> bool { return g_app_globals->vr_mode; }

auto IsStdinATerminal() -> bool { return g_app_globals->is_stdin_a_terminal; }

void ScreenMessage(const std::string& s, const Vector3f& color) {
  if (g_game) {
    g_game->PushScreenMessage(s, color);
  } else {
    Log("ScreenMessage before g_game init (will be lost): '" + s + "'");
  }
}

void ScreenMessage(const std::string& msg) {
  ScreenMessage(msg, {1.0f, 1.0f, 1.0f});
}

auto GetCurrentThreadName() -> std::string {
  return Thread::GetCurrentThreadName();
}

auto IsBootstrapped() -> bool { return g_app_globals->is_bootstrapped; }

// Used by our built in exception type.
void SetPythonException(PyExcType python_type, const char* description) {
  Python::SetPythonException(python_type, description);
}

auto IsUnmodifiedBlessedBuild() -> bool {
  // Assume debug builds are not blessed (we'll determine this after
  // we finish calcing blessing hash, but this we don't get false positives
  // up until that point)
  if (g_buildconfig.debug_build()) {
    return false;
  }

  // Return false if we're unblessed or it seems that the user is likely
  // mucking around with stuff. If we just don't know yet
  // (for instance if blessing has calc hasn't completed) we assume we're
  // clean.
  if (g_app_globals && g_app_globals->user_ran_commands) {
    return false;
  }

  // If they're using custom app scripts, just consider it modified.
  // Otherwise can can tend to get errors in early bootstrapping before
  // we've been able to calc hashes to see if things are modified.
  if (g_platform && g_platform->using_custom_app_python_dir()) {
    return false;
  }

  // If we don't have an embedded blessing hash, we're not blessed. Duh.
  if (kBlessingHash == nullptr) {
    return false;
  }

  // If we have an embedded hash and we've calced ours
  // and it doesn't match, consider ourself modified.
  return !(g_app_globals && !g_app_globals->calced_blessing_hash.empty()
           && g_app_globals->calced_blessing_hash != kBlessingHash);
}

}  // namespace ballistica

// If desired, define main() in the global namespace.
#if BA_DEFINE_MAIN
auto main(int argc, char** argv) -> int {
  return ballistica::BallisticaMain(argc, argv);
}
#endif
