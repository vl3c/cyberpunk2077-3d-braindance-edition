// 3D Braindance Edition: the Fixed 3D settings on Mod Settings' page.
// Nothing reads this class in game: Mod Settings saves it to red4ext\plugins\mod_settings\user.ini when you press Apply,
// and the plugin (braindance3d_framegen.c) reads the file within a second or two. The section is the class's name.
public class BraindanceFixedStereo {

    @runtimeProperty("ModSettings.mod", "3D Braindance Edition")
    @runtimeProperty("ModSettings.displayName", "Fixed 3D")
    @runtimeProperty("ModSettings.description", "Replace the game's self-adjusting stereo with a fixed eye gap and distance from the 3D cinema screen. For true scale, use Immersive with a FoV of 90. SBS only. Press Ctrl+F8 in game to toggle Fixed 3D on and off.")
    let Fixed: Bool = false;

    @runtimeProperty("ModSettings.mod", "3D Braindance Edition")
    @runtimeProperty("ModSettings.displayName", "Your IPD (mm)")
    @runtimeProperty("ModSettings.description", "The real distance between your eyes. The game's Depth setting scales it: Immersive is true scale, Comfort and Balanced are flatter, Extreme is stronger. More stretches the 3D depth, so things beyond the screen go farther away and things in front of it come closer.")
    @runtimeProperty("ModSettings.step", "1.0")
    @runtimeProperty("ModSettings.min", "50.0")
    @runtimeProperty("ModSettings.max", "80.0")
    let EyeGapMm: Float = 65.0;

    @runtimeProperty("ModSettings.mod", "3D Braindance Edition")
    @runtimeProperty("ModSettings.displayName", "Screen size (%)")
    @runtimeProperty("ModSettings.description", "Small screen (0%) feels like the back row at the 3D cinema (30 m distance). Large screen (100%) is like the front row (5 m). It slides the two SBS pictures and doesn't change depth.")
    @runtimeProperty("ModSettings.step", "5.0")
    @runtimeProperty("ModSettings.min", "0.0")
    @runtimeProperty("ModSettings.max", "100.0")
    let DisplaySize: Float = 50.0;
}
