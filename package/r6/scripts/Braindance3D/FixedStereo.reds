// 3D Braindance Edition: the Fixed 3D settings on Mod Settings' page.
// Nothing reads this class in game: Mod Settings saves it to red4ext\plugins\mod_settings\user.ini when you press Apply,
// and the plugin (braindance3d_framegen.c) reads the file within a second or two. The section is the class's name.
// Output is read once, when the game starts (braindance3d_autosetup.c); Mod Settings saves it by the value's name.
enum BraindanceOutput {
    Auto = 0,
    HalfSBS = 1
}

public class BraindanceFixedStereo {

    @runtimeProperty("ModSettings.mod", "3D Braindance Edition")
    @runtimeProperty("ModSettings.displayName", "Fixed 3D")
    @runtimeProperty("ModSettings.description", "Fixed eye gap and screen distance instead of self-adjusting stereo. Try Immersive with FoV 90. Ctrl+F8 toggles it in game.")
    let Fixed: Bool = false;

    @runtimeProperty("ModSettings.mod", "3D Braindance Edition")
    @runtimeProperty("ModSettings.displayName", "Your IPD (mm)")
    @runtimeProperty("ModSettings.description", "The real distance between your eyes. The game's Depth setting scales it: Immersive is true scale.")
    @runtimeProperty("ModSettings.step", "1.0")
    @runtimeProperty("ModSettings.min", "50.0")
    @runtimeProperty("ModSettings.max", "80.0")
    let EyeGapMm: Float = 65.0;

    @runtimeProperty("ModSettings.mod", "3D Braindance Edition")
    @runtimeProperty("ModSettings.displayName", "Screen size (%)")
    @runtimeProperty("ModSettings.description", "0% feels like the back row at the 3D cinema (30 m), 100% like the front row (5 m). Slides the SBS pictures, doesn't change depth.")
    @runtimeProperty("ModSettings.step", "5.0")
    @runtimeProperty("ModSettings.min", "0.0")
    @runtimeProperty("ModSettings.max", "100.0")
    let DisplaySize: Float = 50.0;

    @runtimeProperty("ModSettings.mod", "3D Braindance Edition")
    @runtimeProperty("ModSettings.displayName", "Output")
    @runtimeProperty("ModSettings.description", "Auto: a glasses-free 3D monitor's own 3D when its software is installed, else half SBS. Half SBS: always side by side. Restart the game after Apply.")
    @runtimeProperty("ModSettings.displayValues.Auto", "Auto")
    @runtimeProperty("ModSettings.displayValues.HalfSBS", "Half SBS")
    let Output: BraindanceOutput = BraindanceOutput.Auto;
}
