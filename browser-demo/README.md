# Setting up the browser demo (one-time, ~5 minutes)

Browser Wokwi cannot compile ESP-IDF, so the shareable project runs `sketch.ino` from
this folder — the Arduino-core port. These steps produce the public link for the main
README and the repo homepage.

## Steps

1. Go to https://wokwi.com, **sign in** (saving needs an account), then
   **+ New Project → ESP32** (the plain Arduino ESP32 template — *not* an ESP-IDF one).
2. The editor opens with two tabs: `sketch.ino` and `diagram.json`.
3. Open this folder's [`sketch.ino`](sketch.ino) on GitHub, press **Raw**, select all,
   copy.
4. In Wokwi, click the `sketch.ino` tab, select all, **paste over it**.
   - Pasting over `sketch.ino` is correct *here*: this demo is a single file. (Project
     03's "own tabs" trap was about its *extra* files — there are none in this demo.)
5. **Leave `diagram.json` alone.** The template's ESP32 DevKit-C v4 + serial monitor is
   exactly right. The `diagram.json` in the repo root is for the CI runner — do not
   paste it into the browser project.
6. Press the green **play** button. After the compile, the serial monitor (bottom pane)
   prints the boot banner:

   ```text
   shell: commands: hwm | inv | crash
   ```

7. Try it: click the **input strip at the very BOTTOM of the sim pane** (the narrow
   white bar — easy to miss), type `hwm`, press Enter, and wait for the table ending in
   `hwm: all margins OK`. Then `inv` (takes about a second, ends in
   `inv: inheritance OK`). `crash` works too but panics and reboots the sim — fine to
   demo, just restart afterwards.
8. Name the project **freertos-task-architecture** (click the title in the top bar) and
   **Save**.
9. **Share → copy the link**, then open that link in a private/incognito window and
   check it loads *this* sketch and simulates — an unsaved edit or a private project
   shows the stock blink demo or a login wall.
10. Send the URL back: it goes in the README's "Run it in your browser" line and the
    repo homepage (`gh repo edit DwalloE/freertos-task-architecture --homepage <url>`).

If the compile fails in the browser (Arduino core versions move), copy the first error
back rather than fixing in place — the fix belongs in this folder's `sketch.ino` so the
repo and the saved project never diverge.

## While you're there (the other batched asks)

- `gh secret set WOKWI_CLI_TOKEN --repo DwalloE/freertos-task-architecture` with the
  regenerated 10 Sept token, then re-run the latest CI run.
- The ~10 s recording for `docs/demo.gif`, in this exact order: clear the serial monitor
  (trash-can icon) → **start the screen recording** → press the sim **restart** button so
  the boot scrolls in live → type `hwm`, wait for the table, type `inv`, wait for the
  verdict → stop. (`crash` is best saved for the README, not the gif.)
