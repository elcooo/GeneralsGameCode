/*
**	External AI Player
**	Drives an AI player from outside the engine via two flat files in UserData/:
**	  AiState.json     - state snapshot, rewritten every N frames (engine -> tool)
**	  AiCommands.txt   - line-based command queue, consumed each tick (tool -> engine)
**	Activate by setting environment variable GENERALS_EXTERNAL_AI=1 before launch.
*/

#pragma once

#include "Common/GameMemory.h"
#include "GameLogic/AISkirmishPlayer.h"

class ThingTemplate;

class ExternalAIPlayer : public AISkirmishPlayer
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(ExternalAIPlayer, "ExternalAIPlayer")

public:
	ExternalAIPlayer(Player *p);

	virtual void update() override;
	virtual Bool isSkirmishAI() override { return true; }
	virtual Player *getAiEnemy() override;

	static Bool isEnabledByEnv();

protected:
	virtual void crc(Xfer *xfer) override;
	virtual void xfer(Xfer *xfer) override;
	virtual void loadPostProcess() override;

private:
	void dumpStateJson();
	void processCommandFile();
	void dispatchCommandLine(const char *line);
	Object *startExternalConstruction(const AsciiString &thingName, Bool flank);
	Object *startExternalConstructionAt(const ThingTemplate *thingTemplate, const Coord3D *location, Real angle);
	Bool queueExternalUnit(const AsciiString &thingName);

	UnsignedInt m_lastDumpFrame;
	UnsignedInt m_dumpEveryFrames;
	Bool        m_runBuiltinAI;     // when true, also let AIPlayer::update() run
	Player     *m_cachedEnemy;
};
