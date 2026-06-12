import "CoreLibs/graphics"
import "mcumax"

local gfx = playdate.graphics
local engine = mcumax.new()
local task = engine:begin_search(75000, 7, 32)
local done = false
local best
local info = {nodes = 0, depth = 0, score = 0}

function playdate.update()
    gfx.clear(gfx.kColorWhite)
    gfx.drawText("Incremental search watchdog test", 40, 80)
    gfx.drawText("Nodes: " .. info.nodes, 40, 110)
    gfx.drawText(done and "Complete" or "Searching across frames", 40, 140)

    if not done then
        done, best, info = engine:step_search(task)
        if done then
            assert(best and engine:play_move(best), "search returned an illegal move")
            print("watchdog search complete: " .. mcumax.format_move(best))
        end
    end
end
