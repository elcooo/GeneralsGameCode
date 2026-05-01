/*
**	External AI Player implementation.
*/

#include "PreRTS.h"

#include "Common/GameMemory.h"
#include "Common/BuildAssistant.h"
#include "Common/GlobalData.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Team.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Money.h"
#include "Common/Energy.h"
#include "Common/Xfer.h"
#include "GameClient/TerrainVisual.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/AI.h"
#include "GameLogic/AISkirmishPlayer.h"
#include "GameLogic/ExternalAIPlayer.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/SidesList.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Weapon.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>


// =====================================================================
// activation gate
// =====================================================================
Bool ExternalAIPlayer::isEnabledByEnv()
{
	const char *v = getenv("GENERALS_EXTERNAL_AI");
	return v != nullptr && v[0] == '1';
}

// =====================================================================
// path helpers
// =====================================================================
static AsciiString stateFilePath(int playerIndex)
{
	AsciiString p = TheGlobalData->getPath_UserData();
	char buf[64];
	sprintf(buf, "AiState_p%d.json", playerIndex);
	p.concat(buf);
	return p;
}

static AsciiString commandFilePath(int playerIndex)
{
	AsciiString p = TheGlobalData->getPath_UserData();
	char buf[64];
	sprintf(buf, "AiCommands_p%d.txt", playerIndex);
	p.concat(buf);
	return p;
}

// =====================================================================
// JSON escape
// =====================================================================
static std::string jsonEscape(const char *s)
{
	std::string out;
	if (!s) return out;
	for (; *s; ++s)
	{
		switch (*s)
		{
			case '\\': out += "\\\\"; break;
			case '\"': out += "\\\""; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			default:
				if ((unsigned char)*s < 0x20) { /* skip */ }
				else out += *s;
		}
	}
	return out;
}

static std::string jsonEscape(const AsciiString& s)
{
	return jsonEscape(s.str());
}

static std::string jsonEscape(const UnicodeString& s)
{
	AsciiString ascii;
	ascii.translate(s);
	return jsonEscape(ascii);
}

static void appendExternalAiEvent(Player *player, const char *eventName, const std::string& detailsJson)
{
	if (player == nullptr || TheGlobalData == nullptr || TheGameLogic == nullptr || eventName == nullptr)
		return;

	AsciiString logPath = TheGlobalData->getPath_UserData();
	logPath.concat("AiActionStream.ndjson");

	FILE *fp = fopen(logPath.str(), "a");
	if (fp == nullptr)
		return;

	fprintf(
		fp,
		"{\"frame\":%u,\"playerIndex\":%d,\"player\":\"%s\",\"event\":\"%s\"%s%s}\n",
		(unsigned)TheGameLogic->getFrame(),
		player->getPlayerIndex(),
		jsonEscape(player->getPlayerDisplayName()).c_str(),
		eventName,
		detailsJson.empty() ? "" : ",",
		detailsJson.c_str());
	fflush(fp);
	fclose(fp);
}

static Object *findOwnedProductionFactory(Player *player, const ThingTemplate *unitTemplate, Bool busyOK)
{
	if (player == nullptr || unitTemplate == nullptr || TheGameLogic == nullptr || TheBuildAssistant == nullptr)
		return nullptr;

	Object *busyFactory = nullptr;
	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		if (obj->getControllingPlayer() != player)
			continue;
		if (obj->isEffectivelyDead())
			continue;
		if (obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
			continue;
		if (obj->testStatus(OBJECT_STATUS_SOLD))
			continue;

		ProductionUpdateInterface *production = obj->getProductionUpdateInterface();
		if (!production)
			continue;
		if (TheBuildAssistant->isPossibleToMakeUnit(obj, unitTemplate) == FALSE)
			continue;

		if (production->getProductionCount() == 0)
			return obj;
		if (busyOK)
			busyFactory = obj;
	}

	return busyOK ? busyFactory : nullptr;
}

static Object *findOwnedCommandCenterOrStructure(Player *player)
{
	if (!player || !TheGameLogic)
		return nullptr;

	Object *fallback = nullptr;
	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		if (obj->getControllingPlayer() != player)
			continue;
		if (obj->isEffectivelyDead())
			continue;
		if (!obj->isKindOf(KINDOF_STRUCTURE))
			continue;
		if (obj->isKindOf(KINDOF_COMMANDCENTER))
			return obj;
		if (fallback == nullptr)
			fallback = obj;
	}
	return fallback;
}

// =====================================================================
// ctor
// =====================================================================
ExternalAIPlayer::ExternalAIPlayer(Player *p)
	: AISkirmishPlayer(p),
	  m_lastDumpFrame(0),
	  m_dumpEveryFrames(15),    // ~2x/sec at 30fps
	  m_runBuiltinAI(p && p->getPlayerType() == PLAYER_COMPUTER), // human slots are command-only by default
	  m_cachedEnemy(nullptr)
{
}

ExternalAIPlayer::~ExternalAIPlayer()
{
}

Player *ExternalAIPlayer::getAiEnemy()
{
	// crude: first enemy human we find. The external tool can override targeting.
	if (m_cachedEnemy && !m_cachedEnemy->isPlayerDead())
		return m_cachedEnemy;
	for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player *other = ThePlayerList->getNthPlayer(i);
		if (!other || other == m_player) continue;
		if (other->getRelationship(m_player->getDefaultTeam()) != ENEMIES) continue;
		m_cachedEnemy = other;
		return other;
	}
	return nullptr;
}

// =====================================================================
// main tick
// =====================================================================
void ExternalAIPlayer::update()
{
	UnsignedInt now = TheGameLogic ? TheGameLogic->getFrame() : 0;

	if (now - m_lastDumpFrame >= m_dumpEveryFrames)
	{
		dumpStateJson();
		m_lastDumpFrame = now;
	}

	processCommandFile();

	if (m_runBuiltinAI)
		AISkirmishPlayer::update();
}

// =====================================================================
// state dump (engine -> tool)
// =====================================================================
void ExternalAIPlayer::dumpStateJson()
{
	if (!m_player || !TheGlobalData || !TheGameLogic) return;

	AsciiString path = stateFilePath(m_player->getPlayerIndex());

	// write to a temp then rename so the reader never sees a partial file
	AsciiString tmp = path; tmp.concat(".tmp");

	FILE *fp = fopen(tmp.str(), "w");
	if (!fp) return;

	const Money *money = m_player->getMoney();
	const Energy *energy = m_player->getEnergy();
	Coord3D base; Bool hasBase = getBaseCenter(&base);

	fprintf(fp, "{\n");
	fprintf(fp, "  \"frame\": %u,\n", (unsigned)TheGameLogic->getFrame());
	fprintf(fp, "  \"player\": {\n");
	fprintf(fp, "    \"index\": %d,\n", m_player->getPlayerIndex());
	fprintf(fp, "    \"name\": \"%s\",\n",
		jsonEscape(m_player->getPlayerDisplayName()).c_str());
	fprintf(fp, "    \"money\": %d,\n", money ? money->countMoney() : 0);
	fprintf(fp, "    \"energyProduction\": %d,\n", energy ? energy->getProduction() : 0);
	fprintf(fp, "    \"energyConsumption\": %d,\n", energy ? energy->getConsumption() : 0);
	if (hasBase)
		fprintf(fp, "    \"baseCenter\": [%.1f, %.1f],\n", base.x, base.y);
	fprintf(fp, "    \"runBuiltinAI\": %s\n", m_runBuiltinAI ? "true" : "false");
	fprintf(fp, "  },\n");

	// owned objects
	fprintf(fp, "  \"units\": [\n");
	Bool first = true;
	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		if (obj->getControllingPlayer() != m_player) continue;
		if (obj->isEffectivelyDead()) continue;

		const Coord3D *pos = obj->getPosition();
		const ThingTemplate *tt = obj->getTemplate();
		if (!first) fprintf(fp, ",\n");
		first = false;
		fprintf(fp,
			"    {\"id\":%u,\"type\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"hp\":%.0f,\"isStructure\":%s}",
			(unsigned)obj->getID(),
			jsonEscape(tt ? tt->getName().str() : "").c_str(),
			pos ? pos->x : 0.0f,
			pos ? pos->y : 0.0f,
			obj->getBodyModule() ? obj->getBodyModule()->getHealth() : 0.0f,
			obj->isKindOf(KINDOF_STRUCTURE) ? "true" : "false");
	}
	fprintf(fp, "\n  ],\n");

	// visible enemies (anything not on our team that we can see)
	fprintf(fp, "  \"enemies\": [\n");
	first = true;
	for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		Player *owner = obj->getControllingPlayer();
		if (!owner || owner == m_player) continue;
		if (owner->getRelationship(m_player->getDefaultTeam()) != ENEMIES) continue;
		if (obj->isEffectivelyDead()) continue;
		// only emit unfogged for this player
		if (obj->getShroudedStatus(m_player->getPlayerIndex()) >= OBJECTSHROUD_FOGGED) continue;

		const Coord3D *pos = obj->getPosition();
		const ThingTemplate *tt = obj->getTemplate();
		if (!first) fprintf(fp, ",\n");
		first = false;
		fprintf(fp,
			"    {\"id\":%u,\"type\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"owner\":\"%s\"}",
			(unsigned)obj->getID(),
			jsonEscape(tt ? tt->getName().str() : "").c_str(),
			pos ? pos->x : 0.0f,
			pos ? pos->y : 0.0f,
			jsonEscape(owner->getPlayerDisplayName()).c_str());
	}
	fprintf(fp, "\n  ],\n");

	// teams owned by this player
	fprintf(fp, "  \"teams\": [\n");
	first = true;
	const Player::PlayerTeamList *teams = m_player->getPlayerTeams();
	if (teams)
	{
		for (Player::PlayerTeamList::const_iterator it = teams->begin(); it != teams->end(); ++it)
		{
			TeamPrototype *proto = *it;
			if (!proto) continue;
			Int instances = proto->countTeamInstances();
			if (!first) fprintf(fp, ",\n");
			first = false;
			fprintf(fp,
				"    {\"name\":\"%s\",\"priority\":%d,\"maxInstances\":%d,\"instances\":%d}",
				jsonEscape(proto->getName().str()).c_str(),
				proto->getTemplateInfo() ? proto->getTemplateInfo()->m_productionPriority : 0,
				proto->getTemplateInfo() ? proto->getTemplateInfo()->m_maxInstances : 0,
				instances);
		}
	}
	fprintf(fp, "\n  ],\n");

	// base-build plan owned by this player
	fprintf(fp, "  \"buildList\": [\n");
	first = true;
	for (BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext())
	{
		AsciiString name = info->getTemplateName();
		if (name.isEmpty()) continue;

		ObjectID objectID = info->getObjectID();
		Object *builtObject = TheGameLogic->findObjectByID(objectID);
		if (!first) fprintf(fp, ",\n");
		first = false;
		fprintf(fp,
			"    {\"template\":\"%s\",\"objectId\":%u,\"built\":%s,\"priority\":%s}",
			jsonEscape(name).c_str(),
			(unsigned)objectID,
			builtObject ? "true" : "false",
			info->isPriorityBuild() ? "true" : "false");
	}
	fprintf(fp, "\n  ]\n");

	fprintf(fp, "}\n");
	fclose(fp);

	// atomic-ish swap
	remove(path.str());
	rename(tmp.str(), path.str());
}

// =====================================================================
// command intake (tool -> engine)
// =====================================================================
void ExternalAIPlayer::processCommandFile()
{
	if (!m_player) return;

	AsciiString path = commandFilePath(m_player->getPlayerIndex());
	FILE *fp = fopen(path.str(), "r");
	if (!fp) return;

	char line[1024];
	while (fgets(line, sizeof(line), fp))
	{
		// strip trailing newline
		size_t len = strlen(line);
		while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
			line[--len] = '\0';
		if (len == 0 || line[0] == '#') continue;

		dispatchCommandLine(line);
	}
	fclose(fp);

	// consume the file so the same commands don't run twice
	remove(path.str());
}

// =====================================================================
// command dispatch
// =====================================================================
static const char *skipSpaces(const char *p) { while (*p == ' ' || *p == '\t') ++p; return p; }

static AsciiString takeToken(const char *&p)
{
	p = skipSpaces(p);
	const char *start = p;
	while (*p && *p != ' ' && *p != '\t') ++p;
	return AsciiString(std::string(start, p - start).c_str());
}

Object *ExternalAIPlayer::startExternalConstructionAt(
	const ThingTemplate *thingTemplate,
	const Coord3D *location,
	Real angle)
{
	if (!thingTemplate || !location)
		return nullptr;

	BuildListInfo *info = newInstance(BuildListInfo);
	info->setTemplateName(thingTemplate->getName());
	info->setBuildingName(thingTemplate->getName());
	info->setLocation(*location);
	info->setAngle(angle);
	info->setNumRebuilds(1);

	Object *building = buildStructureWithDozer(thingTemplate, info);
	deleteInstance(info);

	if (building)
	{
		appendExternalAiEvent(
			m_player,
			"structure_started",
			"\"building\":\"" + jsonEscape(thingTemplate->getName()) + "\"," +
			"\"objectId\":" + std::to_string((unsigned)building->getID()));
	}
	return building;
}

Object *ExternalAIPlayer::startExternalConstruction(const AsciiString &thingName, Bool flank)
{
	if (thingName.isEmpty() || !m_player || !TheThingFactory)
		return nullptr;

	const ThingTemplate *thingTemplate = TheThingFactory->findTemplate(thingName);
	if (!thingTemplate)
		return nullptr;

	// First try the map/skirmish build-list positions. They usually match the
	// intended base layout and avoid crowding the command center.
	for (BuildListInfo *info = m_player->getBuildList(); info; info = info->getNext())
	{
		if (info->getTemplateName() != thingName)
			continue;
		Object *existing = TheGameLogic ? TheGameLogic->findObjectByID(info->getObjectID()) : nullptr;
		if (existing)
			continue;
		Object *building = startExternalConstructionAt(thingTemplate, info->getLocation(), info->getAngle());
		if (building)
			return building;
	}

	Coord3D base;
	if (!getBaseCenter(&base))
	{
		Object *anchor = findOwnedCommandCenterOrStructure(m_player);
		if (anchor)
			base = *anchor->getPosition();
		else
			base.zero();
	}

	const Real angle = thingTemplate->getPlacementViewAngle();
	const Real radii[] = { 180.0f, 260.0f, 360.0f, 480.0f, 620.0f, 800.0f };
	const Real dirs[][2] = {
		{ 1.0f, 0.0f }, { 0.0f, 1.0f }, { -1.0f, 0.0f }, { 0.0f, -1.0f },
		{ 0.7f, 0.7f }, { -0.7f, 0.7f }, { -0.7f, -0.7f }, { 0.7f, -0.7f }
	};

	for (size_t r = 0; r < ARRAY_SIZE(radii); ++r)
	{
		for (size_t d = 0; d < ARRAY_SIZE(dirs); ++d)
		{
			size_t dirIndex = flank ? (d + 2) % ARRAY_SIZE(dirs) : d;
			Coord3D pos = base;
			pos.x += dirs[dirIndex][0] * radii[r];
			pos.y += dirs[dirIndex][1] * radii[r];
			pos.z = 0.0f;
			Object *building = startExternalConstructionAt(thingTemplate, &pos, angle);
			if (building)
				return building;
		}
	}

	appendExternalAiEvent(
		m_player,
		"structure_failed",
		"\"building\":\"" + jsonEscape(thingName) + "\"");
	return nullptr;
}

Bool ExternalAIPlayer::queueExternalUnit(const AsciiString &thingName)
{
	if (thingName.isEmpty() || !m_player || !TheThingFactory)
		return false;

	const ThingTemplate *unitTemplate = TheThingFactory->findTemplate(thingName);
	if (!unitTemplate)
		return false;

	Object *factory = findFactory(unitTemplate, true);
	if (!factory)
		factory = findOwnedProductionFactory(m_player, unitTemplate, true);
	if (!factory)
	{
		appendExternalAiEvent(
			m_player,
			"train_failed",
			"\"unit\":\"" + jsonEscape(thingName) + "\",\"reason\":\"no_factory\"");
		return false;
	}

	ProductionUpdateInterface *production = factory->getProductionUpdateInterface();
	if (!production || !production->queueCreateUnit(unitTemplate, production->requestUniqueUnitID()))
	{
		appendExternalAiEvent(
			m_player,
			"train_failed",
			"\"unit\":\"" + jsonEscape(thingName) + "\",\"reason\":\"queue_rejected\"");
		return false;
	}

	appendExternalAiEvent(
		m_player,
		"train_queued",
		"\"unit\":\"" + jsonEscape(thingName) + "\"," +
		"\"factory\":\"" + jsonEscape(factory->getTemplate()->getName()) + "\"");
	return true;
}

void ExternalAIPlayer::dispatchCommandLine(const char *line)
{
	const char *p = line;
	AsciiString cmd = takeToken(p);

	// ---- meta ----
	if (cmd == "PAUSE_BUILTIN")  { m_runBuiltinAI = false; return; }
	if (cmd == "RESUME_BUILTIN") { m_runBuiltinAI = true;  return; }
	if (cmd == "DUMP_RATE")
	{
		AsciiString n = takeToken(p);
		Int frames = atoi(n.str());
		if (frames > 0) m_dumpEveryFrames = (UnsignedInt)frames;
		return;
	}

	// ---- strategic ----
	if (cmd == "BUILD_BUILDING")
	{
		AsciiString name = takeToken(p);
		if (!name.isEmpty())
		{
			if (m_player && m_player->getPlayerType() == PLAYER_HUMAN)
				startExternalConstruction(name, false);
			else
				buildSpecificAIBuilding(name);
		}
		return;
	}
	if (cmd == "BUILD_BASE_DEFENSE")
	{
		AsciiString name = takeToken(p);
		AsciiString flank = takeToken(p);
		Bool flankBool = (flank == "1" || flank == "flank");
		if (m_player && m_player->getPlayerType() == PLAYER_HUMAN)
		{
			if (!name.isEmpty())
				startExternalConstruction(name, flankBool);
		}
		else
		{
			if (name.isEmpty()) buildAIBaseDefense(flankBool);
			else                buildAIBaseDefenseStructure(name, flankBool);
		}
		return;
	}
	if (cmd == "BUILD_TEAM" || cmd == "RECRUIT_TEAM")
	{
		AsciiString protoName = takeToken(p);
		TeamPrototype *proto = TheTeamFactory ? TheTeamFactory->findTeamPrototype(protoName) : nullptr;
		if (!proto) return;
		if (cmd == "BUILD_TEAM")
		{
			buildSpecificAITeam(proto, /*priorityBuild*/ true);
		}
		else
		{
			AsciiString r = takeToken(p);
			Real radius = (Real)atof(r.str());
			if (radius <= 0) radius = 200.0f;
			recruitSpecificAITeam(proto, radius);
		}
		return;
	}
	if (cmd == "BUILD_UPGRADE")
	{
		AsciiString name = takeToken(p);
		if (!name.isEmpty())
		{
			appendExternalAiEvent(
				m_player,
				"upgrade_requested",
				"\"upgrade\":\"" + jsonEscape(name) + "\"");
			buildUpgrade(name);
		}
		return;
	}
	if (cmd == "TRAIN_UNIT")
	{
		AsciiString name = takeToken(p);
		if (!name.isEmpty()) queueExternalUnit(name);
		return;
	}

	// ---- unit-level (id-addressed) ----
	if (cmd == "MOVE" || cmd == "ATTACK_MOVE" || cmd == "GUARD_POSITION" ||
	    cmd == "ATTACK" || cmd == "HUNT" || cmd == "IDLE")
	{
		AsciiString idStr = takeToken(p);
		ObjectID id = (ObjectID)atoi(idStr.str());
		Object *obj = TheGameLogic ? TheGameLogic->findObjectByID(id) : nullptr;
		if (!obj || obj->getControllingPlayer() != m_player) return;
		AIUpdateInterface *ai = obj->getAIUpdateInterface();
		if (!ai) return;

		if (cmd == "HUNT") { ai->aiHunt(CMD_FROM_AI); return; }
		if (cmd == "IDLE") { ai->aiIdle(CMD_FROM_AI); return; }

		if (cmd == "ATTACK")
		{
			AsciiString vStr = takeToken(p);
			ObjectID vid = (ObjectID)atoi(vStr.str());
			Object *victim = TheGameLogic->findObjectByID(vid);
			if (victim) ai->aiAttackObject(victim, NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);
			return;
		}

		AsciiString xs = takeToken(p);
		AsciiString ys = takeToken(p);
		Coord3D pos; pos.x = (Real)atof(xs.str()); pos.y = (Real)atof(ys.str()); pos.z = 0;

		if (cmd == "MOVE")            ai->aiMoveToPosition(&pos, CMD_FROM_AI);
		else if (cmd == "ATTACK_MOVE") ai->aiAttackMoveToPosition(&pos, NO_MAX_SHOTS_LIMIT, CMD_FROM_AI);
		else if (cmd == "GUARD_POSITION") ai->aiGuardPosition(&pos, GUARDMODE_NORMAL, CMD_FROM_AI);
		return;
	}

	// unknown -> log and ignore
	DEBUG_LOG(("ExternalAIPlayer: unknown command '%s'\n", line));
}

// =====================================================================
// snapshot stubs (delegate to base; we hold no save-relevant state)
// =====================================================================
void ExternalAIPlayer::crc(Xfer *xfer)             { AISkirmishPlayer::crc(xfer); }
void ExternalAIPlayer::xfer(Xfer *xfer)            { AISkirmishPlayer::xfer(xfer); }
void ExternalAIPlayer::loadPostProcess()           { AISkirmishPlayer::loadPostProcess(); }
