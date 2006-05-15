import sys, os.path

# SConscript_opts exports PortAudio options
optsDict = SConscript(os.path.join("scons", "SConscript_opts"))
optionsCache = os.path.join("scons", "options.cache")   # Save options between runs in this cache
options = Options(optionsCache, args=ARGUMENTS)
for k in ("Installation Dirs", "Library Types", "Host APIs", "Build Parameters"):
    options.AddOptions(*optsDict[k])
# Propagate options into environment
env = Environment(options=options)
# Save options for next run
options.Save(optionsCache, env)
# Generate help text for options
env.Help(options.GenerateHelpText(env))

buildDir = os.path.join("#", "scons", "build")

# Don't change directory when running the SConscript
env.SConscriptChdir(False)
sources, sharedLib, staticLib, tests, portEnv = env.SConscript("SConscript", build_dir=buildDir, duplicate=False, exports=["env"])
# Build these by default
env.Default(sharedLib, staticLib, tests)

"""
if env["enablePython"]:
    env.SConscriptChdir(True)
    pyEnv = env.Copy()
    pyEnv["CCFLAGS"] = portEnv["CCFLAGS"].replace("-pedantic", "")
    module = env.SConscript("python/SConscript", exports={"env": pyEnv, "BuildDir": BuildDir})
    env.Default(module)
    """
