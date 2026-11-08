package com.slithymatt.gyreandgimble;

import org.libsdl.app.SDLActivity;

/** Gyre & Gimble: SDL runs the game (libmain.so, from the root CMakeLists.txt). */
public class GyreActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }
}
