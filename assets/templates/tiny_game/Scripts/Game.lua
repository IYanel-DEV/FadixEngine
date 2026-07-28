-- TinyGame level 1. Shows the gameplay FXS API:
--   World.find(name)          -> locate an entity by NameComponent
--   Prefab.spawn(path,x,y,z)  -> instantiate a .prefab in the play world
--   Scene.load(path)          -> transition to another scene
-- Runs identically in editor Play and the exported fadix_player.

function OnStart(entity)
    local player = World.find("Player")
    if player then
        print("TinyGame: found Player (id " .. tostring(player.id) .. ")")
    else
        print("TinyGame: Player not found")
    end

    -- Drop a pickup prefab just in front of the player.
    local pickup = Prefab.spawn("Prefabs/Pickup.prefab", 0.0, 0.5, -2.0)
    if pickup then
        print("TinyGame: spawned Pickup")
    end

    print("TinyGame: Level 1 ready - press N to load Level 2.")
end

function OnUpdate(entity, deltaTime)
    if Input.isDown("N") then
        Scene.load("Scenes/Level2.scene")
    end
end
