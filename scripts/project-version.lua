-- All product version metadata comes from version.txt. This runs in script
-- scope because Xmake's description scope has no file-reading API.
function main(target)
    local version = assert(io.readfile(path.join(os.projectdir(), "version.txt"))):trim()
    local major, minor, patch = version:match("^(%d+)%.(%d+)%.(%d+)$")
    assert(major and tonumber(major) <= 65535 and tonumber(minor) <= 65535
        and tonumber(patch) <= 65535, "invalid version.txt: expected MAJOR.MINOR.PATCH (0..65535)")
    target:set("version", version)
    target:add("defines", 'SU_VERSION_STR="' .. version .. '"')

    local directory = path.join(target:autogendir(), "version")
    local header = path.join(directory, "su_version.h")
    local content = '#pragma once\n'
        .. '// Generated from version.txt; do not edit.\n'
        .. '#define SU_VERSION_NUMERIC ' .. major .. ',' .. minor .. ',' .. patch .. ',0\n'
        .. '#ifndef SU_VERSION_STR\n#define SU_VERSION_STR "' .. version .. '"\n#endif\n'
    os.mkdir(directory)
    if not os.isfile(header) or io.readfile(header) ~= content then
        io.writefile(header, content)
    end
    target:add("includedirs", directory)
end
