-- Embed the repository's actual notices and license texts so the About
-- page also works offline, in an unpackaged build or after relocation.
function main(output)
    import("core.base.json")
    local root = os.projectdir()
    local data = json.loadfile(path.join(root, "assets", "about", "credits.json"))
    data.documents = {}
    local function add_document(label, file)
        local text = assert(io.readfile(file))
        -- The two original CeCILL texts are ISO-8859-1. Slint strings and
        -- JSON require UTF-8; keep the originals intact in licenses/.
        if label == "CImg-CeCILL-C.txt" or label == "CImg-CeCILL-V2.txt" then
            text = text:gsub("[\128-\255]", function(character)
                local byte = character:byte()
                return string.char(192 + math.floor(byte / 64), 128 + byte % 64)
            end)
        end
        table.insert(data.documents, {name = label, text = text})
    end
    add_document("Smile2Unlock / MIT", path.join(root, "LICENSE"))
    add_document("Third-party notices", path.join(root, "NOTICE", "THIRD-PARTY-NOTICES.md"))
    local files = os.files(path.join(root, "licenses", "**"))
    table.sort(files)
    for _, file in ipairs(files) do
        local label = path.relative(file, path.join(root, "licenses")):gsub("\\", "/")
        add_document(label, file)
    end
    local encoded = json.encode(data)
    local delimiter = "SU_ABOUT"
    while encoded:find(")" .. delimiter .. '"', 1, true) do
        delimiter = delimiter .. "_"
    end
    local header = '#pragma once\n#include <string_view>\n'
        .. 'inline constexpr char su_about_json_data[] = R"' .. delimiter
        .. '(' .. encoded .. ')' .. delimiter .. '";\n'
        .. 'inline constexpr std::string_view su_about_json{su_about_json_data, sizeof(su_about_json_data) - 1};\n'
    os.mkdir(path.directory(output))
    if not os.isfile(output) or io.readfile(output) ~= header then
        io.writefile(output, header)
    end
end
