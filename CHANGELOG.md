# Changelog

## 1.0.1

- Integrate PR #1's default controller mode, ZL mouse toggle, stick menu navigation, Minus start touch, and offline console-clock synchronization.
- Release virtual directions and neutralize analog axes when changing input modes; preserve overlapping physical D-pad holds.
- Normalize touchscreen input independently of the configured rendering resolution.
- Deliver offline time-sync success callbacks after the request returns, using the game's original listener dispatcher.
- Keep the cursor hidden while mouse mode is disabled, including docked launches.
- Coordinate Minus, cursor clicks and real touch under a single pointer owner.
- Resolve static SDL/Mesa dependencies through the Switch pkg-config wrapper so current toolchains link complete release builds.
- Add native input and time-sync regression tests.

## 1.0.0

- Initial Nintendo Switch port of Jetpack Joyride 1.104.1 for Android ARM64.
