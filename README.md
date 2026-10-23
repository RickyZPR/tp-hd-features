# TP HD Features

> [!IMPORTANT]
> **Unofficial fan project. Not affiliated with, endorsed by, or supported by the Dusklight team (TwilitRealm) or by Nintendo.**
>
> - Please don't ask the Dusklight team for help with this mod, and don't report its bugs to them. [Open an issue here](../../issues) instead.
> - This project was made with a lot of help from an AI (Anthropic's Claude). Dusklight does not accept contributions that are primarily AI-generated, so this is not a contribution to Dusklight. It is kept completely separate, out of respect for that rule.
> - It is a fan tool for personal use with games you own. There are no Nintendo files here: you make the game files on your own PC, from your own copies of the games.
> - I have great respect for the Dusklight team. Thank you for Dusklight.
>
> Please read the full [disclaimer](DISCLAIMER.md).

An unofficial fan mod for [Dusklight](https://twilitrealm.dev/), a PC port of *Twilight Princess* (GameCube).
It adds two things from *Twilight Princess HD*: the 50 stamps and the Cave of Shadows.
In the game, the mod is called **HD Features**.

## What it adds

**Stamps**
- HD's 50 stamps: 46 in chests (7 of them are HD's extra chests), plus the ones from Agitha, Gengle and the Malo Mart, and Twili Midna at the end.
- HD's stamp book on the Collection screen, and your stamp count on the file select.

**Cave of Shadows**
- HD's 41-floor Wolf Link dungeon, in three sections.
- You get in with a virtual Wolf Link amiibo: press Z on the Collection screen. No figure needed.
- HD's record screen and its rewards, up to the Colossal Wallet.

You can turn each part on or off in Dusklight's Mods window.
Details: [stamps](mods/hdfeatures/docs/stamps.md) and [Cave of Shadows](mods/hdfeatures/docs/cave.md).

## For players

**You need:**
- Windows 10 or 11, 64-bit.
- [Dusklight](https://twilitrealm.dev/) v2.0.3, set up with your own *Twilight Princess* GameCube disc (USA version, GZ2E01).
- Your own *Twilight Princess HD* for Wii U with its v81 update, the way Cemu uses it: a game folder (code, content, meta) or a `.wua` file.
- About 200 MB of free disk space.

Nothing else: no Python, and nothing to run.

**Install:**
1. Set up Dusklight 2.0.3 with your USA GameCube disc.
2. Have your TP HD as a game folder or a `.wua`, the way Cemu uses it. A `.wux` or `.wud` must be converted to `.wua` in Cemu once.
3. Download `hd_features-2.1.0.dusk` from [Releases](../../releases). Put it in Dusklight's `mods` folder (`%APPDATA%\TwilitRealm\Dusklight\mods`), or drag it onto the Dusklight window.
4. Start Dusklight. The first time, HD Features finds your TP HD and makes its game files from it (usually under a minute). If it can't find it, it asks once: open Mods > HD Features and choose your TP HD folder.

Back up your saves first. Dusklight keeps them in its data folder (normally `%APPDATA%\TwilitRealm\Dusklight`).
The game files are made from your own games and stay on your PC, so never share them. To share the mod, share the link to this page.
Keep your TP HD: a later version of HD Features may need it again.

**Uninstall:**
1. Optional, to free about 75 MB: in Mods > HD Features > Troubleshooting, press "Delete the game files". They are removed when Dusklight closes.
2. Close Dusklight. Delete the HD Features `.dusk` from `%APPDATA%\TwilitRealm\Dusklight\mods`.
3. If you skipped step 1, you can delete the folder `%APPDATA%\TwilitRealm\Dusklight\mod_data\local.ricky.hdfeatures` instead.

Uninstalling does not change your saves.

## For developers

How to build the mod and its converter, run the tests and make the release: [BUILDING.md](BUILDING.md).
Want to help? See [CONTRIBUTING.md](CONTRIBUTING.md).

## Support

Something wrong? [Open an issue here](../../issues).
Please **never** ask the Dusklight team, and never post about this mod in Dusklight's issues, pull requests or community channels.
It helps to say what the "Game files" line in Mods > HD Features says, and to attach Dusklight's newest log (in `%APPDATA%\TwilitRealm\Dusklight\logs`).
Remove your user name and folder names from it first. Never attach game files or the `mod_data` folder.

## Thanks

- **The Dusklight team (TwilitRealm)**, for Dusklight and its mod SDK. This mod would not exist without their work.
- **The zeldaret/tp decompilation contributors**, whose work Dusklight is built on.
- Font and code credits: [NOTICE.md](NOTICE.md).

## License

This project's own code is under the [MIT License](LICENSE).
*Twilight Princess* and everything made from the games belong to Nintendo, and none of it is in this repository.
