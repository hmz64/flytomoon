# FlyToMoon — FlyMe2theMoon Raylib recreation (miHoYo 2011 homage)

64-bit Android (`arm64-v8a`) + desktop build from a single `main.cpp` (Raylib, C++).

- Touch LEFT half = thrust up-right, RIGHT half = up-left (original `applyPush:pushPoint:` logic)
- Physics reverse-engineered from `roleConfig.json` (`maxF 28, maxP 51, damping 0.1, yGravity -10`, ...)
- 3 handcrafted levels + Survival endless, mana/fuel, stars, moon gate, 60k/70k score colors
- Width-locked camera handles 16:9 and 20:9 phones,Portrait-first

## Desktop (GCC)

```bash
sudo apt install build-essential libraylib-dev
g++ main.cpp -o flyme2themoon -std=c++17 -O2 -lraylib -lm -lpthread -ldl -lrt -lX11
./flyme2themoon
```

## Android APK via GitHub Actions

Push to `main` triggers `.github/workflows/build-apk.yml`:
`arm64-v8a` debug APK built with AGP 8.5 + NDK 25 + CMake 3.22 + Raylib 5.5,
uploaded as artifact `flytomoon-debug-apk`.

Download from Actions tab → Artifacts, or via `gh run download`.
