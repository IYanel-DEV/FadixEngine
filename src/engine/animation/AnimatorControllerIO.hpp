#pragma once

#include "engine/animation/AnimationClip.hpp"

#include <algorithm>
#include <iomanip>
#include <istream>
#include <ostream>
#include <utility>

namespace fadix
{
inline void WriteAnimatorControllerData(std::ostream& out, const AnimatorController& controller)
{
    out << std::quoted(controller.Name) << ' ' << std::quoted(controller.EntryState) << ' '
        << controller.Parameters.size() << ' ' << controller.States.size() << ' '
        << controller.Transitions.size() << ' ';
    for (const AnimatorParameter& parameter : controller.Parameters)
    {
        out << std::quoted(parameter.Name) << ' ' << static_cast<int>(parameter.Type) << ' '
            << parameter.BoolValue << ' ' << parameter.FloatValue << ' ' << parameter.IntValue
            << ' ';
    }
    for (const AnimatorState& state : controller.States)
    {
        out << std::quoted(state.Name) << ' ' << std::quoted(state.ClipName) << ' '
            << state.Position.x << ' ' << state.Position.y << ' ';
    }
    for (const AnimatorTransition& transition : controller.Transitions)
    {
        out << std::quoted(transition.From) << ' ' << std::quoted(transition.To) << ' '
            << transition.Duration << ' ' << transition.HasExitTime << ' ' << transition.ExitTime
            << ' ' << transition.Conditions.size() << ' ';
        for (const AnimatorCondition& condition : transition.Conditions)
        {
            out << std::quoted(condition.Parameter) << ' '
                << static_cast<int>(condition.Comparison) << ' ' << condition.Threshold << ' ';
        }
    }
}

inline bool ReadAnimatorControllerData(std::istream& row, AnimatorController& controller)
{
    std::size_t parameterCount = 0;
    std::size_t stateCount = 0;
    std::size_t transitionCount = 0;
    row >> std::quoted(controller.Name) >> std::quoted(controller.EntryState) >> parameterCount >>
        stateCount >> transitionCount;
    controller.Parameters.clear();
    controller.States.clear();
    controller.Transitions.clear();
    for (std::size_t i = 0; i < parameterCount && row; ++i)
    {
        AnimatorParameter parameter;
        int type = 0;
        row >> std::quoted(parameter.Name) >> type >> parameter.BoolValue >> parameter.FloatValue >>
            parameter.IntValue;
        parameter.Type = static_cast<AnimatorParameterType>(std::clamp(type, 0, 3));
        controller.Parameters.push_back(std::move(parameter));
    }
    for (std::size_t i = 0; i < stateCount && row; ++i)
    {
        AnimatorState state;
        row >> std::quoted(state.Name) >> std::quoted(state.ClipName) >> state.Position.x >>
            state.Position.y;
        controller.States.push_back(std::move(state));
    }
    for (std::size_t i = 0; i < transitionCount && row; ++i)
    {
        AnimatorTransition transition;
        int comparison = 0;
        std::size_t conditionCount = 0;
        row >> std::quoted(transition.From) >> std::quoted(transition.To) >> transition.Duration >>
            transition.HasExitTime >> transition.ExitTime >> conditionCount;
        transition.Duration = std::max(transition.Duration, 0.0F);
        transition.ExitTime = std::clamp(transition.ExitTime, 0.0F, 1.0F);
        for (std::size_t c = 0; c < conditionCount && row; ++c)
        {
            AnimatorCondition condition;
            row >> std::quoted(condition.Parameter) >> comparison >> condition.Threshold;
            condition.Comparison =
                static_cast<AnimatorComparison>(std::clamp(comparison, 0, 3));
            transition.Conditions.push_back(std::move(condition));
        }
        controller.Transitions.push_back(std::move(transition));
    }
    return static_cast<bool>(row);
}
}
