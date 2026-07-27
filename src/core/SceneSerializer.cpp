#include "core/SceneSerializer.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Components.hpp"
#include "core/Entity.hpp"
#include "core/Scene.hpp"

// =============================================================================
// Fadix Engine — SceneSerializer Implementation
//
// Single source of truth for the .fadix / .fadixscene JSON format.
// Component coverage: Tag, Transform, Relationship (parent index), Mesh,
// Light, Camera, RigidBody3D, BoxCollider, SphereCollider, Script (editor
// asset binding + property overrides), NativeScript bound-flag advisory.
// =============================================================================

namespace {

using nlohmann::json;

json Vec3ToJson(const glm::vec3& v)
{
    return json::array({ v.x, v.y, v.z });
}

glm::vec3 JsonToVec3(const json& parent, const char* key, const glm::vec3& fallback)
{
    if (!parent.contains(key)) return fallback;
    const json& a = parent[key];
    if (!a.is_array() || a.size() < 3) return fallback;
    return glm::vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
}

} // namespace

// =============================================================================
// Construction
// =============================================================================

SceneSerializer::SceneSerializer(const std::shared_ptr<Scene>& scene)
    : m_Scene(scene)
{
}

// =============================================================================
// Serialize — registry → .fadix JSON file
// =============================================================================

bool SceneSerializer::Serialize(const std::string& filepath)
{
    if (!m_Scene) return false;

    json root;
    root["fadix"]       = 3;
    json& entitiesArray = root["entities"] = json::array();

    const entt::registry& reg = m_Scene->GetRegistry();

    // Stable entity order + handle → array-index map for parent references.
    std::vector<entt::entity> ordered;
    for (const auto handle : reg.view<TagComponent>())
        ordered.push_back(handle);
    std::reverse(ordered.begin(), ordered.end()); // view iterates newest-first

    std::unordered_map<std::uint32_t, int> indexOf;
    for (int i = 0; i < static_cast<int>(ordered.size()); ++i)
        indexOf[entt::to_integral(ordered[static_cast<std::size_t>(i)])] = i;

    for (const auto handle : ordered)
    {
        json je;
        je["tag"] = reg.get<TagComponent>(handle).Name;

        // -- Relationship (parent as array index) ----------------------------
        if (const auto* rel = reg.try_get<RelationshipComponent>(handle))
        {
            if (rel->Parent != entt::null)
            {
                const auto it = indexOf.find(entt::to_integral(rel->Parent));
                if (it != indexOf.end())
                    je["parent"] = it->second;
            }
        }

        // -- TransformComponent ----------------------------------------------
        if (const auto* tc = reg.try_get<TransformComponent>(handle))
        {
            je["transform"] = {
                { "position", Vec3ToJson(tc->Position) },
                { "rotation", Vec3ToJson(tc->Rotation) },
                { "scale",    Vec3ToJson(tc->Scale)    },
            };
        }

        // -- MeshComponent ---------------------------------------------------
        if (const auto* mc = reg.try_get<MeshComponent>(handle))
        {
            je["mesh"] = {
                { "filePath",       mc->FilePath       },
                { "castShadows",    mc->CastShadows    },
                { "receiveShadows", mc->ReceiveShadows },
            };
        }

        // -- LightComponent --------------------------------------------------
        if (const auto* lc = reg.try_get<LightComponent>(handle))
        {
            je["light"] = {
                { "type",              static_cast<int>(lc->Type) },
                { "intensity",         lc->Intensity              },
                { "color",             Vec3ToJson(lc->Color)      },
                { "attenuationRadius", lc->AttenuationRadius      },
                { "innerConeAngle",    lc->InnerConeAngle         },
                { "outerConeAngle",    lc->OuterConeAngle         },
            };
        }

        // -- CameraComponent -------------------------------------------------
        if (const auto* cc = reg.try_get<CameraComponent>(handle))
        {
            je["camera"] = {
                { "projection", static_cast<int>(cc->Projection) },
                { "fov",        cc->FieldOfView                  },
                { "nearClip",   cc->NearClip                     },
                { "farClip",    cc->FarClip                      },
            };
        }

        // -- RigidBody3DComponent (Jolt) ------------------------------------------
        if (const auto* rb = reg.try_get<RigidBody3DComponent>(handle))
        {
            je["rigidBody3d"] = {
                { "motionType",    static_cast<int>(rb->MotionType) },
                { "mass",          rb->Mass           },
                { "friction",      rb->Friction       },
                { "restitution",   rb->Restitution    },
                { "gravityFactor", rb->GravityFactor  },
                { "linearDamping", rb->LinearDamping  },
                { "angularDamping",rb->AngularDamping },
            };
        }

        // -- BoxCollider3DComponent (Jolt) -------------------------------------
        if (const auto* bc = reg.try_get<BoxCollider3DComponent>(handle))
        {
            je["boxCollider3d"] = {
                { "halfExtents", Vec3ToJson(bc->HalfExtents) },
                { "friction",    bc->Friction    },
                { "restitution", bc->Restitution },
                { "density",     bc->Density     },
            };
        }

        // -- SphereCollider3DComponent (Jolt) ----------------------------------
        if (const auto* sc = reg.try_get<SphereCollider3DComponent>(handle))
        {
            je["sphereCollider3d"] = {
                { "radius",      sc->Radius      },
                { "friction",    sc->Friction    },
                { "restitution", sc->Restitution },
                { "density",     sc->Density     },
            };
        }

        // -- RigidBody2DComponent (Box2D v3) -----------------------------------
        if (const auto* rb2 = reg.try_get<RigidBody2DComponent>(handle))
        {
            je["rigidBody2d"] = {
                { "motionType",    static_cast<int>(rb2->MotionType) },
                { "fixedRotation", rb2->FixedRotation  },
                { "gravityScale",  rb2->GravityScale   },
                { "linearDamping", rb2->LinearDamping  },
                { "angularDamping",rb2->AngularDamping },
            };
        }

        // -- BoxCollider2DComponent (Box2D v3) ---------------------------------
        if (const auto* bc2 = reg.try_get<BoxCollider2DComponent>(handle))
        {
            je["boxCollider2d"] = {
                { "halfExtents", { bc2->HalfExtents.x, bc2->HalfExtents.y } },
                { "friction",    bc2->Friction    },
                { "restitution", bc2->Restitution },
                { "density",     bc2->Density     },
            };
        }

#ifdef FADIX_EDITOR
        // -- ScriptComponent (editor asset binding + property overrides) ------
        if (const auto* sc = reg.try_get<ScriptComponent>(handle))
        {
            json jscript;
            jscript["assetPath"] = sc->ScriptAssetPath;
            jscript["assetUUID"] = sc->ScriptAssetId.ToString();

            json jov = json::object();
            for (const auto& [name, val] : sc->PropertyOverrides)
            {
                json jv;
                jv["type"] = static_cast<int>(val.Type);
                jv["i"]    = val.IntVal;
                jv["f"]    = val.FloatVal;
                jv["v"]    = Vec3ToJson(val.Vec3Val);
                jv["s"]    = val.StringVal;
                jov[name]  = std::move(jv);
            }
            jscript["overrides"] = std::move(jov);
            je["scriptAsset"]    = std::move(jscript);
        }
#endif

        // -- NativeScriptComponent -------------------------------------------
        // Function-pointer bindings are not serialisable across process
        // boundaries, so we record whether a script was attached at save time.
        if (const auto* nsc = reg.try_get<NativeScriptComponent>(handle))
        {
            je["script"] = {
                { "isBound", nsc->InstantiateScript != nullptr },
            };
        }

        entitiesArray.push_back(std::move(je));
    }

    try
    {
        const std::filesystem::path path(filepath);
        if (path.has_parent_path())
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
        }

        std::ofstream out(path);
        if (!out.is_open()) return false;

        out << root.dump(4) << '\n';
        return out.good();
    }
    catch (...)
    {
        return false;
    }
}

// =============================================================================
// Deserialize — .fadix JSON file → registry
// =============================================================================

bool SceneSerializer::Deserialize(const std::string& filepath)
{
    if (!m_Scene) return false;

    std::ifstream in(filepath);
    if (!in.is_open()) return false;

    const json root = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object() ||
        !root.contains("entities") || !root["entities"].is_array())
    {
        return false;
    }

    m_Scene->Clear();

    try
    {
        // Pass 1 — create entities and load their components.
        std::vector<entt::entity> created;
        std::vector<int>          parentIndices;

        for (const auto& je : root["entities"])
        {
            if (!je.is_object()) continue;

            const std::string tag = je.value("tag", std::string("Entity"));
            Entity entity = m_Scene->CreateEntity(tag);
            created.push_back(entity.GetHandle());
            parentIndices.push_back(je.value("parent", -1));

            // -- TransformComponent ------------------------------------------
            if (je.contains("transform") && je["transform"].is_object())
            {
                const json& jt = je["transform"];
                auto& tc = entity.GetComponent<TransformComponent>();
                tc.Position = JsonToVec3(jt, "position", tc.Position);
                tc.Rotation = JsonToVec3(jt, "rotation", tc.Rotation);
                tc.Scale    = JsonToVec3(jt, "scale",    tc.Scale);
            }

            // -- MeshComponent -----------------------------------------------
            if (je.contains("mesh") && je["mesh"].is_object())
            {
                const json& jm = je["mesh"];
                auto& mc          = entity.AddComponent<MeshComponent>();
                mc.FilePath       = jm.value("filePath",       mc.FilePath);
                mc.CastShadows    = jm.value("castShadows",    mc.CastShadows);
                mc.ReceiveShadows = jm.value("receiveShadows", mc.ReceiveShadows);
            }

            // -- LightComponent ----------------------------------------------
            if (je.contains("light") && je["light"].is_object())
            {
                const json& jl = je["light"];
                auto& lc             = entity.AddComponent<LightComponent>();
                lc.Type              = static_cast<LightType>(
                                           std::clamp(jl.value("type", 1), 0, 2));
                lc.Intensity         = jl.value("intensity",         lc.Intensity);
                lc.Color             = JsonToVec3(jl, "color",       lc.Color);
                lc.AttenuationRadius = jl.value("attenuationRadius", lc.AttenuationRadius);
                lc.InnerConeAngle    = jl.value("innerConeAngle",    lc.InnerConeAngle);
                lc.OuterConeAngle    = jl.value("outerConeAngle",    lc.OuterConeAngle);
            }

            // -- CameraComponent ---------------------------------------------
            if (je.contains("camera") && je["camera"].is_object())
            {
                const json& jc = je["camera"];
                auto& cc        = entity.AddComponent<CameraComponent>();
                cc.Projection   = static_cast<ProjectionType>(
                                      std::clamp(jc.value("projection", 0), 0, 1));
                cc.FieldOfView  = jc.value("fov",      cc.FieldOfView);
                cc.NearClip     = jc.value("nearClip", cc.NearClip);
                cc.FarClip      = jc.value("farClip",  cc.FarClip);
            }

            // -- RigidBody3DComponent (Jolt) ---------------------------------------
            if (je.contains("rigidBody3d") && je["rigidBody3d"].is_object())
            {
                const json& jr = je["rigidBody3d"];
                auto& rb          = entity.AddComponent<RigidBody3DComponent>();
                rb.MotionType     = static_cast<MotionType3D>(
                                        std::clamp(jr.value("motionType", 2), 0, 2));
                rb.Mass           = jr.value("mass",          rb.Mass);
                rb.Friction       = jr.value("friction",      rb.Friction);
                rb.Restitution    = jr.value("restitution",   rb.Restitution);
                rb.GravityFactor  = jr.value("gravityFactor", rb.GravityFactor);
                rb.LinearDamping  = jr.value("linearDamping", rb.LinearDamping);
                rb.AngularDamping = jr.value("angularDamping",rb.AngularDamping);
            }

            // -- BoxCollider3DComponent (Jolt) ---------------------------------
            if (je.contains("boxCollider3d") && je["boxCollider3d"].is_object())
            {
                const json& jb = je["boxCollider3d"];
                auto& bc       = entity.AddComponent<BoxCollider3DComponent>();
                bc.HalfExtents = JsonToVec3(jb, "halfExtents", bc.HalfExtents);
                bc.Friction    = jb.value("friction",    bc.Friction);
                bc.Restitution = jb.value("restitution", bc.Restitution);
                bc.Density     = jb.value("density",     bc.Density);
            }

            // -- SphereCollider3DComponent (Jolt) ------------------------------
            if (je.contains("sphereCollider3d") && je["sphereCollider3d"].is_object())
            {
                const json& js = je["sphereCollider3d"];
                auto& sc       = entity.AddComponent<SphereCollider3DComponent>();
                sc.Radius      = js.value("radius",      sc.Radius);
                sc.Friction    = js.value("friction",    sc.Friction);
                sc.Restitution = js.value("restitution", sc.Restitution);
                sc.Density     = js.value("density",     sc.Density);
            }

            // -- RigidBody2DComponent (Box2D v3) -------------------------------
            if (je.contains("rigidBody2d") && je["rigidBody2d"].is_object())
            {
                const json& jr2 = je["rigidBody2d"];
                auto& rb2          = entity.AddComponent<RigidBody2DComponent>();
                rb2.MotionType     = static_cast<MotionType2D>(
                                         std::clamp(jr2.value("motionType", 2), 0, 2));
                rb2.FixedRotation  = jr2.value("fixedRotation", rb2.FixedRotation);
                rb2.GravityScale   = jr2.value("gravityScale",  rb2.GravityScale);
                rb2.LinearDamping  = jr2.value("linearDamping", rb2.LinearDamping);
                rb2.AngularDamping = jr2.value("angularDamping",rb2.AngularDamping);
            }

            // -- BoxCollider2DComponent (Box2D v3) -----------------------------
            if (je.contains("boxCollider2d") && je["boxCollider2d"].is_object())
            {
                const json& jb2 = je["boxCollider2d"];
                auto& bc2       = entity.AddComponent<BoxCollider2DComponent>();
                if (jb2.contains("halfExtents") && jb2["halfExtents"].is_array()
                    && jb2["halfExtents"].size() >= 2)
                {
                    bc2.HalfExtents.x = jb2["halfExtents"][0].get<float>();
                    bc2.HalfExtents.y = jb2["halfExtents"][1].get<float>();
                }
                bc2.Friction    = jb2.value("friction",    bc2.Friction);
                bc2.Restitution = jb2.value("restitution", bc2.Restitution);
                bc2.Density     = jb2.value("density",     bc2.Density);
            }

#ifdef FADIX_EDITOR
            // -- ScriptComponent ----------------------------------------------
            if (je.contains("scriptAsset") && je["scriptAsset"].is_object())
            {
                const json& jscript = je["scriptAsset"];
                auto& sc           = entity.AddComponent<ScriptComponent>();
                sc.ScriptAssetPath = jscript.value("assetPath", std::string{});
                sc.ScriptAssetId   = fadix::UUID::FromString(
                                         jscript.value("assetUUID", std::string{}));

                if (jscript.contains("overrides") && jscript["overrides"].is_object())
                {
                    for (const auto& [name, jv] : jscript["overrides"].items())
                    {
                        fadix::PropertyValue val;
                        val.Type = static_cast<fadix::PropertyType>(jv.value(
                            "type", static_cast<int>(fadix::PropertyType::Float)));
                        val.IntVal    = jv.value("i", 0);
                        val.FloatVal  = jv.value("f", 0.0f);
                        val.Vec3Val   = JsonToVec3(jv, "v", glm::vec3(0.0f));
                        val.StringVal = jv.value("s", std::string{});
                        sc.PropertyOverrides[name] = std::move(val);
                    }
                }
            }
#endif

            // -- NativeScriptComponent advisory ------------------------------
            // The binding cannot be reconstructed from disk — emit a console
            // note so the developer knows to re-attach the script manually.
            if (je.contains("script") && je["script"].is_object())
            {
                if (je["script"].value("isBound", false))
                {
                    std::cout << "[SceneSerializer] Advisory: entity \""
                              << tag
                              << "\" had a NativeScriptComponent bound at save "
                                 "time. Re-call Bind<T>() after loading to "
                                 "restore the script.\n";
                }
            }
        }

        // Pass 2 — wire parent/child relationships now that every entity exists.
        for (std::size_t i = 0; i < created.size(); ++i)
        {
            const int parentIdx = parentIndices[i];
            if (parentIdx >= 0 && parentIdx < static_cast<int>(created.size()) &&
                parentIdx != static_cast<int>(i))
            {
                m_Scene->SetParent(created[i],
                                   created[static_cast<std::size_t>(parentIdx)]);
            }
        }
        return true;
    }
    catch (...)
    {
        m_Scene->Clear();
        return false;
    }
}
