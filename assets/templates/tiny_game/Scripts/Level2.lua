-- TinyGame level 2. Reached via Scene.load from level 1.
-- Press B to go back to level 1, proving the transition works both ways.

function OnStart(entity)
    local trophy = World.find("Trophy")
    if trophy then
        print("TinyGame: Level 2 reached - found the Trophy!")
    end
end

function OnUpdate(entity, deltaTime)
    if Input.isDown("B") then
        Scene.load("Scenes/Main.scene")
    end
end
