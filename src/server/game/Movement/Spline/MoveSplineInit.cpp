/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/

#include "MovementPacketBuilder.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "Opcodes.h"
#include "Transport.h"
#include "Unit.h"
#include "Vehicle.h"
#include "WorldPacket.h"
#include "MotionMaster.h"
#include "WatchMgr.h"
#include "GridDefines.h"
#include "Log.h"
#include <sstream>

namespace Movement
{
    UnitMoveType SelectSpeedType(uint32 moveFlags)
    {
        if (moveFlags & MOVEMENTFLAG_FLYING)
        {
            if (moveFlags & MOVEMENTFLAG_BACKWARD /*&& speed_obj.flight >= speed_obj.flight_back*/)
                return MOVE_FLIGHT_BACK;
            else
                return MOVE_FLIGHT;
        }
        else if (moveFlags & MOVEMENTFLAG_SWIMMING)
        {
            if (moveFlags & MOVEMENTFLAG_BACKWARD /*&& speed_obj.swim >= speed_obj.swim_back*/)
                return MOVE_SWIM_BACK;
            else
                return MOVE_SWIM;
        }
        else if (moveFlags & MOVEMENTFLAG_WALKING)
        {
            //if (speed_obj.run > speed_obj.walk)
            return MOVE_WALK;
        }
        else if (moveFlags & MOVEMENTFLAG_BACKWARD /*&& speed_obj.run >= speed_obj.run_back*/)
            return MOVE_RUN_BACK;

        // Flying creatures use MOVEMENTFLAG_CAN_FLY or MOVEMENTFLAG_DISABLE_GRAVITY
        // Run speed is their default flight speed.
        return MOVE_RUN;
    }

    // [lab] WatchMgr 'move' category: report every creature spline the server sends to clients,
    // so a server<->client position desync ("ghost" mobs) can be traced to the exact move that
    // caused it (chase/jump/home/waypoint, from->to, duration). Cheap early-out when no watch.
    static void WatchMove(Unit* unit, char const* ev, MoveSplineInitArgs const& args, int32 duration)
    {
        if (!sWatchMgr->Active() || unit->GetTypeId() != TypeID::TYPEID_UNIT)
            return;
        std::ostringstream ss;
        ss << ",\"mg\":" << uint32(unit->GetMotionMaster()->GetCurrentMovementGeneratorType())
           << ",\"dur\":" << duration
           << ",\"vel\":" << args.velocity
           << ",\"pts\":" << args.path.size();
        if (!args.path.empty())
        {
            Vector3 const& a = args.path.front();
            Vector3 const& b = args.path.back();
            ss << ",\"from\":[" << a.x << "," << a.y << "," << a.z << "]"
               << ",\"to\":[" << b.x << "," << b.y << "," << b.z << "]";
        }
        ss << ",\"parabolic\":" << (args.flags.parabolic ? 1 : 0)
           << ",\"walk\":" << (args.flags.walkmode ? 1 : 0)
           << ",\"cyclic\":" << (args.flags.cyclic ? 1 : 0);
        sWatchMgr->Event(unit, WATCH_CAT_MOVE, ev, ss.str());
    }

    // [lab] Spline-position guard. A unit's in-flight spline can end up evaluating to a garbage
    // position (seen live: Cobalt Serpents in Heroic Sethekk with ComputePosition().x ~ -5.3e8
    // while y/z and the unit's real map position were sane). Launch() seeds every NEW spline from
    // ComputePosition(), so once corrupted the garbage start is copied into each following move
    // forever: chase splines from x=-5e8 with duration INT32_MAX. The server can't relocate the
    // unit there (it stays frozen at its last valid spot) while the client is fed nonsense, and
    // you get a "ghost": visible and targetable but "out of range / not in line of sight" at
    // point-blank range. Fall back to the unit's real position when the computed one is invalid
    // or implausibly far from it, which heals the unit on its next move. Logged with the spline
    // dump so the original corruption can still be traced.
    static float const SPLINE_POS_MAX_DRIFT = 50.0f;
    static bool SaneSplinePosition(Unit* unit, Location const& p)
    {
        return Skyfire::IsValidMapCoord(p.x, p.y, p.z)
            && unit->GetExactDist(p.x, p.y, p.z) < SPLINE_POS_MAX_DRIFT;
    }

    static void RepairSplinePosition(Unit* unit, MoveSpline const& ms, Location& p, char const* where)
    {
        // Invalid coordinates are real corruption. A valid but distant position is usually just a
        // stale map position (e.g. a patrol whose spline kept advancing while its grid was idle),
        // so only note that at debug level. Either way, restart from the unit's map position.
        if (!Skyfire::IsValidMapCoord(p.x, p.y, p.z))
            SF_LOG_ERROR("misc", "[lab] %s: discarded corrupt spline position (%f, %f, %f) for %s entry %u guid %u at (%f, %f, %f); spline: %s",
                where, p.x, p.y, p.z, unit->GetTypeId() == TypeID::TYPEID_UNIT ? "creature" : "unit", unit->GetEntry(), unit->GetGUIDLow(),
                unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZMinusOffset(), ms.ToString().c_str());
        else
            SF_LOG_DEBUG("misc", "[lab] %s: spline position (%f, %f, %f) is %.0fyd from entry %u guid %u map position; restarting from map position",
                where, p.x, p.y, p.z, unit->GetExactDist(p.x, p.y, p.z), unit->GetEntry(), unit->GetGUIDLow());
        p = Location(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZMinusOffset(), unit->GetOrientation());
    }

    int32 MoveSplineInit::Launch()
    {
        MoveSpline& move_spline = *unit->movespline;

        Location real_position(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZMinusOffset(), unit->GetOrientation());
        // Elevators also use MOVEMENTFLAG_ONTRANSPORT but we do not keep track of their position changes
        if (unit->GetTransGUID())
        {
            real_position.x = unit->GetTransOffsetX();
            real_position.y = unit->GetTransOffsetY();
            real_position.z = unit->GetTransOffsetZ();
            real_position.orientation = unit->GetTransOffsetO();
        }

        // there is a big chance that current position is unknown if current state is not finalized, need compute it
        // this also allows calculate spline position and update map position in much greater intervals
        // Don't compute for transport movement if the unit is in a motion between two transports
        if (!move_spline.Finalized() && move_spline.onTransport == (unit->GetTransGUID() != 0))
        {
            real_position = move_spline.ComputePosition();
            if (!unit->GetTransGUID() && !SaneSplinePosition(unit, real_position))
                RepairSplinePosition(unit, move_spline, real_position, "MoveSplineInit::Launch");
        }

        // should i do the things that user should do? - no.
        if (args.path.empty())
            return 0;

        // correct first vertex
        args.path[0] = real_position;
        args.initialOrientation = real_position.orientation;
        move_spline.onTransport = (unit->GetTransGUID() != 0);

        uint32 moveFlags = unit->m_movementInfo.GetMovementFlags();
        moveFlags |= MOVEMENTFLAG_FORWARD;

        if (moveFlags & MOVEMENTFLAG_ROOT)
            moveFlags &= ~MOVEMENTFLAG_MASK_MOVING;

        if (!args.HasVelocity)
        {
            // If spline is initialized with SetWalk method it only means we need to select
            // walk move speed for it but not add walk flag to unit
            uint32 moveFlagsForSpeed = moveFlags;
            if (args.flags.walkmode)
                moveFlagsForSpeed |= MOVEMENTFLAG_WALKING;
            else
                moveFlagsForSpeed &= ~MOVEMENTFLAG_WALKING;

            args.velocity = unit->GetSpeed(SelectSpeedType(moveFlagsForSpeed));
        }

        if (!args.Validate(unit))
        {
            WatchMove(unit, "MOVE_REJECTED", args, 0);
            return 0;
        }

        unit->m_movementInfo.SetMovementFlags(moveFlags);
        move_spline.Initialize(args);

        WorldPacket data(SMSG_ON_MONSTER_MOVE, 64);
        PacketBuilder::WriteMonsterMove(move_spline, data, unit);
        unit->SendMessageToSet(&data, true);

        WatchMove(unit, "MOVE_LAUNCH", args, move_spline.Duration());
        return move_spline.Duration();
    }

    void MoveSplineInit::Stop()
    {
        MoveSpline& move_spline = *unit->movespline;

        // No need to stop if we are not moving
        if (move_spline.Finalized())
            return;

        Location loc = move_spline.ComputePosition();
        if (!unit->GetTransGUID() && !SaneSplinePosition(unit, loc))
            RepairSplinePosition(unit, move_spline, loc, "MoveSplineInit::Stop");
        args.flags = MoveSplineFlag::Done;
        unit->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_FORWARD);
        move_spline.Initialize(args);

        WorldPacket data(SMSG_ON_MONSTER_MOVE, 64);

        PacketBuilder::WriteStopMovement(loc, args.splineId, data, unit);
        unit->SendMessageToSet(&data, true);

        if (sWatchMgr->Active() && unit->GetTypeId() == TypeID::TYPEID_UNIT)
        {
            std::ostringstream ss;
            ss << ",\"mg\":" << uint32(unit->GetMotionMaster()->GetCurrentMovementGeneratorType())
               << ",\"at\":[" << loc.x << "," << loc.y << "," << loc.z << "]";
            sWatchMgr->Event(unit, WATCH_CAT_MOVE, "MOVE_STOP", ss.str());
        }
    }

    MoveSplineInit::MoveSplineInit(Unit* m) : unit(m)
    {
        args.splineId = splineIdGen.NewId();
        // Elevators also use MOVEMENTFLAG_ONTRANSPORT but we do not keep track of their position changes
        args.TransformForTransport = unit->GetTransGUID();
        // mix existing state into new
        args.flags.walkmode = unit->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_WALKING);
        args.flags.flying = unit->m_movementInfo.HasMovementFlag(MovementFlags(MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY));
        args.flags.smoothGroundPath = true; // enabled by default, CatmullRom mode or client config "pathSmoothing" will disable this
    }

    void MoveSplineInit::SetFacing(const Unit* target)
    {
        args.flags.EnableFacingTarget();
        args.facing.target = target->GetGUID();
    }

    void MoveSplineInit::SetFacing(float angle)
    {
        if (args.TransformForTransport)
        {
            if (Unit* vehicle = unit->GetVehicleBase())
                angle -= vehicle->GetOrientation();
            else if (Transport* transport = unit->GetTransport())
                angle -= transport->GetOrientation();
        }

        args.facing.angle = G3D::wrap(angle, 0.f, (float)G3D::twoPi());
        args.flags.EnableFacingAngle();
    }

    void MoveSplineInit::MoveTo(const Vector3& dest, bool generatePath, bool forceDestination)
    {
        if (generatePath)
        {
            PathGenerator path(unit);
            bool result = path.CalculatePath(dest.x, dest.y, dest.z, forceDestination);
            if (result && !(path.GetPathType() & PATHFIND_NOPATH))
            {
                MovebyPath(path.GetPath());
                return;
            }
        }

        args.path_Idx_offset = 0;
        args.path.resize(2);
        TransportPathTransform transform(unit, args.TransformForTransport);
        args.path[1] = transform(dest);
    }

    void MoveSplineInit::SetFall()
    {
        args.flags.EnableFalling();
        args.flags.fallingSlow = unit->HasUnitMovementFlag(MOVEMENTFLAG_FALLING_SLOW);
    }

    Vector3 TransportPathTransform::operator()(Vector3 input)
    {
        if (_transformForTransport)
            if (TransportBase* transport = _owner->GetDirectTransport())
                transport->CalculatePassengerOffset(input.x, input.y, input.z);

        return input;
    }
}
