#include "World.h"
#include "../Common/CommonFunctions.h"
#include "../Common/Version.h"
#include "../Configuration/Configuration.h"
#include "Characters/Character.h"
#include "Date.h"
#include "Log.h"
#include "OSCompatibilityLayer.h"
#include "Offmaps/Offmap.h"
#include "ParserHelpers.h"
#include "Titles/Liege.h"
#include "Titles/Title.h"
#include <ZipFile.h>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

CK2::World::World(const Configuration& theConfiguration)
{
	LOG(LogLevel::Info) << "*** Hello CK2, Deus Vult! ***";
	registerKeyword("CK2txt", [](const std::string& unused, std::istream& theStream) {});
	registerKeyword("date", [this](const std::string& unused, std::istream& theStream) {
		const commonItems::singleString dateString(theStream);
		endDate = date(dateString.getString());
	});
	registerKeyword("start_date", [this](const std::string& unused, std::istream& theStream) {
		const commonItems::singleString startDateString(theStream);
		startDate = date(startDateString.getString());
	});
	registerKeyword("flags", [this](const std::string& unused, std::istream& theStream) {
		// We're not interested in flags. We're here for one thing only.
		const auto flagsItem = commonItems::singleItem(unused, theStream);
		if (flagsItem.find("aztec_explorers") != std::string::npos) {
			// Ho boy.
			invasion = true;
			LOG(LogLevel::Info) << "oO Invasion detected. We're in for a ride!";
		}
	});
	registerKeyword("version", [this](const std::string& unused, std::istream& theStream) {
		const commonItems::singleString versionString(theStream);
		CK2Version = Version(versionString.getString());
		Log(LogLevel::Info) << "<> Savegame version: " << versionString.getString();
	});
	registerKeyword("provinces", [this](const std::string& unused, std::istream& theStream) {
		LOG(LogLevel::Info) << "-> Loading Provinces";
		provinces = Provinces(theStream);
		LOG(LogLevel::Info) << ">> Loaded " << provinces.getProvinces().size() << " provinces.";
	});
	registerKeyword("character", [this](const std::string& unused, std::istream& theStream) {
		LOG(LogLevel::Info) << "-> Loading Characters";
		characters = Characters(theStream);
		LOG(LogLevel::Info) << ">> Loaded " << characters.getCharacters().size() << " characters.";
	});
	registerKeyword("title", [this](const std::string& unused, std::istream& theStream) {
		LOG(LogLevel::Info) << "-> Loading Titles";
		titles = Titles(theStream);
		LOG(LogLevel::Info) << ">> Loaded " << titles.getTitles().size() << " titles.";
	});
	registerKeyword("dynasties", [this](const std::string& unused, std::istream& theStream) {
		LOG(LogLevel::Info) << "-> Loading Dynasties";
		dynasties.loadDynasties(theStream);
		LOG(LogLevel::Info) << ">> Loaded " << dynasties.getDynasties().size() << " dynasties.";
	});
	registerKeyword("wonder", [this](const std::string& unused, std::istream& theStream) {
		LOG(LogLevel::Info) << "-> Loading Wonders";
		wonders = Wonders(theStream);
		LOG(LogLevel::Info) << ">> Loaded " << wonders.getWonders().size() << " wonders.";
	});
	registerKeyword("offmap_powers", [this](const std::string& unused, std::istream& theStream) {
		LOG(LogLevel::Info) << "-> Loading Offmaps";
		offmaps = Offmaps(theStream);
		LOG(LogLevel::Info) << ">> Loaded " << offmaps.getOffmaps().size() << " offmaps.";
	});
	registerKeyword("dyn_title", [this](const std::string& unused, std::istream& theStream) {
		const auto dynTitle = Liege(theStream);
		dynamicTitles.insert(std::pair(dynTitle.getTitle().first, dynTitle));
	});
	registerKeyword("relation", [this](const std::string& unused, std::istream& theStream) {
		LOG(LogLevel::Info) << "-> Loading Diplomacy";
		diplomacy = Diplomacy(theStream);
		LOG(LogLevel::Info) << ">> Loaded " << diplomacy.getDiplomacy().size() << " personal diplomacies.";
	});

	registerRegex("[A-Za-z0-9\\_]+", commonItems::ignoreItem);

	LOG(LogLevel::Info) << "-> Verifying CK2 save.";
	verifySave(theConfiguration.getSaveGamePath());

	LOG(LogLevel::Info) << "-> Importing CK2 save.";
	if (!saveGame.compressed) {
		std::ifstream inBinary(fs::u8path(theConfiguration.getSaveGamePath()), std::ios::binary);
		if (!inBinary.is_open()) {
			LOG(LogLevel::Error) << "Could not open " << theConfiguration.getSaveGamePath() << " for parsing.";
			throw std::runtime_error("Could not open " + theConfiguration.getSaveGamePath() + " for parsing.");
		}
		std::stringstream inStream;
		inStream << inBinary.rdbuf();
		saveGame.gamestate = inStream.str();
	}

	// We must load initializers before the savegame.
	std::set<std::string> fileNames;
	Utils::GetAllFilesInFolder(theConfiguration.getCK2Path() + "/common/dynasties/", fileNames);
	for (const auto& file: fileNames)
		dynasties.loadDynasties(theConfiguration.getCK2Path() + "/common/dynasties/" + file);
	personalityScraper.scrapePersonalities(theConfiguration);

	auto gameState = std::istringstream(saveGame.gamestate);
	parseStream(gameState);
	clearRegisteredKeywords();
	LOG(LogLevel::Info) << ">> Loaded " << dynamicTitles.size() << " dynamic titles.";
	LOG(LogLevel::Info) << "-> Importing Province Titles";
	provinceTitleMapper.loadProvinces(theConfiguration.getCK2Path());


	LOG(LogLevel::Info) << "*** Building World ***";

	// Link all the intertwining pointers
	LOG(LogLevel::Info) << "-- Filtering Excess Province Titles";
	provinceTitleMapper.filterSelf(provinces, titles);
	LOG(LogLevel::Info) << "-- Linking Characters With Dynasties";
	characters.linkDynasties(dynasties);
	LOG(LogLevel::Info) << "-- Linking Characters With Lieges and Spouses";
	characters.linkLiegesAndSpouses();
	LOG(LogLevel::Info) << "-- Linking Characters With Family";
	characters.linkMothersAndFathers();
	LOG(LogLevel::Info) << "-- Linking Characters With Primary Titles";
	characters.linkPrimaryTitles(titles);
	LOG(LogLevel::Info) << "-- Linking Characters With Capitals";
	characters.linkCapitals(provinces);
	LOG(LogLevel::Info) << "-- Linking Provinces With Primary Baronies";
	provinces.linkPrimarySettlements();
	LOG(LogLevel::Info) << "-- Linking Provinces With Wonders";
	provinces.linkWonders(wonders);
	LOG(LogLevel::Info) << "-- Linking Titles With Holders";
	titles.linkHolders(characters);
	LOG(LogLevel::Info) << "-- Linking Titles With Previous Holders";
	titles.linkPreviousHolders(characters);
	LOG(LogLevel::Info) << "-- Linking Titles With Liege and DeJure Titles";
	titles.linkLiegePrimaryTitles();
	LOG(LogLevel::Info) << "-- Linking Titles With Vassals and DeJure Vassals";
	titles.linkVassals();
	LOG(LogLevel::Info) << "-- Linking Titles With Provinces";
	titles.linkProvinces(provinces, provinceTitleMapper); // Untestable due to disk access.
	LOG(LogLevel::Info) << "-- Linking Titles With Base Titles";
	titles.linkBaseTitles();
	LOG(LogLevel::Info) << "-- Linking The Celestial Emperor";
	linkCelestialEmperor();

	// Intermezzo
	verifyReligionsAndCultures(theConfiguration);

	// Filter top-tier active titles and assign them provinces.
	LOG(LogLevel::Info) << "-- Merging Independent Baronies";
	mergeIndependentBaronies();
	LOG(LogLevel::Info) << "-- Merging Revolts Into Base";
	titles.mergeRevolts();
	LOG(LogLevel::Info) << "-- Shattering HRE";
	shatterHRE(theConfiguration);
	LOG(LogLevel::Info) << "-- Shattering Empires";
	shatterEmpires(theConfiguration);
	LOG(LogLevel::Info) << "-- Filtering Independent Titles";
	filterIndependentTitles();
	LOG(LogLevel::Info) << "-- Splitting Off Vassals";
	splitVassals();
	LOG(LogLevel::Info) << "-- Rounding Up Some People";
	gatherCourtierNames();
	LOG(LogLevel::Info) << "-- Congregating Provinces for Independent Titles";
	congregateProvinces();
	LOG(LogLevel::Info) << "-- Performing Province Sanity Check";
	sanityCheckifyProvinces();
	LOG(LogLevel::Info) << "-- Filtering Provinceless Titles";
	filterProvincelessTitles();
	LOG(LogLevel::Info) << "-- Determining Heirs";
	determineHeirs();
	LOG(LogLevel::Info) << "-- Decyphering Personalities";
	characters.assignPersonalities(personalityScraper);

	LOG(LogLevel::Info) << "*** Good-bye CK2, rest in peace. ***";
}

void CK2::World::verifyReligionsAndCultures(const Configuration& theConfiguration)
{
	auto insanityCounter = 0;
	LOG(LogLevel::Info) << "-- Verifyling All Characters Have Religion And Culture Loaded";
	for (const auto& character: characters.getCharacters()) {
		if (character.second->getReligion().empty() || character.second->getCulture().empty()) insanityCounter++;
	}
	if (!insanityCounter) {
		Log(LogLevel::Info) << "<> All " << characters.getCharacters().size() << "characters are sane.";
		return;
	}
	Log(LogLevel::Warning) << "! " << insanityCounter << " characters have lacking definitions! Attempting recovery.";
	loadDynastiesFromMods(theConfiguration);
}

void CK2::World::loadDynastiesFromMods(const Configuration& theConfiguration)
{
	LOG(LogLevel::Info) << "*** Intermezzo ***";
	Log(LogLevel::Info) << "-> Locating mods in mod folder";
	mods.loadModDirectory(theConfiguration);
	Log(LogLevel::Info) << "-> Rummaging through mods in search of definitions.";
	bool weAreSane = false;
	for (const auto& mod: mods.getMods()) {
		if (Utils::doesFolderExist(mod.second + "/common/dynasties/")) {
			Log(LogLevel::Info) << "Found something interesting in " << mod.first;
			std::set<std::string> fileNames;
			Utils::GetAllFilesInFolder(mod.second + "/common/dynasties/", fileNames);
			for (const auto& file: fileNames)
				dynasties.underLoadDynasties(mod.second + "/common/dynasties/" + file);
		} else
			continue;
		auto insanityCounter = 0;
		for (const auto& character: characters.getCharacters()) {
			if (character.second->getReligion().empty() || character.second->getCulture().empty()) insanityCounter++;
		}
		if (!insanityCounter) {
			Log(LogLevel::Info) << "<> All " << characters.getCharacters().size() << " characters have been sanified. Cancelling rummage.";
			weAreSane = true;
			break;
		}
		Log(LogLevel::Warning) << "! " << insanityCounter << " characters are still lacking definitions. Continuing with the rummage.";
	}

	if (!weAreSane) LOG(LogLevel::Warning) << "... We did what we could.";
	LOG(LogLevel::Info) << "*** Intermezzo End, back to scheduled run ***";
}


void CK2::World::linkCelestialEmperor() const
{
	const auto& china = offmaps.getChina();
	if (!china) {
		LOG(LogLevel::Info) << ">< No China detected.";
		return;
	}
	if (!china->second->getHolder().first) {
		LOG(LogLevel::Info) << ">< China has no emperor.";
		return;
	}
	const auto& chars = characters.getCharacters();
	const auto& characterItr = chars.find(china->second->getHolder().first);
	if (characterItr == chars.end()) {
		LOG(LogLevel::Info) << ">< Celestial emperor has no definition!";
		return;
	}
	china->second->setHolder(std::pair(characterItr->first, characterItr->second));
	const auto& holder = china->second->getHolder();
	if (!holder.second->getDynasty().first) {
		LOG(LogLevel::Info) << ">< Celestial emperor has no dynasty!";
		return;
	}
	const auto& dyns = dynasties.getDynasties();
	const auto& dynastyItr = dyns.find(holder.second->getDynasty().first);
	if (dynastyItr == dyns.end()) {
		LOG(LogLevel::Info) << ">< Celestial emperor's dynasty has no definition!";
		return;
	}
	holder.second->setDynasty(dynastyItr->second);
	LOG(LogLevel::Info) << "<> One Celestial Emperor linked.";
}

void CK2::World::determineHeirs()
{
	// We're doing this one late as the number of people involved is reduced by thousandfold.
	for (const auto& title: independentTitles) {
		const auto& holder = title.second->getHolder();
		auto law = title.second->getSuccessionLaw();
		auto gender = title.second->getGenderLaw();

		if (law == "primogeniture" || law == "elective_gavelkind" || law == "gavelkind" || law == "nomad_succession")
			resolvePrimogeniture(gender, holder);
		else if (law == "ultimogeniture")
			resolveUltimogeniture(gender, holder);
		else if (law == "tanistry" || law == "eldership")
			resolveTanistry(gender, holder);
		else if (law == "turkish_succession")
			resolveTurkish(holder);
	}
	LOG(LogLevel::Info) << "<> Heirs resolved where possible.";
}

void CK2::World::resolveTurkish(const std::pair<int, std::shared_ptr<Character>>& holder) const
{
	auto children = holder.second->getChildren();
	std::vector<std::pair<int, std::shared_ptr<Character>>> childVector;

	// instead of filtering by id, we're filtering by raw prestige.
	for (const auto& child: children)
		childVector.emplace_back(std::pair(lround(child.second->getPrestige()), child.second));
	std::sort(childVector.begin(), childVector.end());

	for (const auto& child: childVector) {
		if (child.second->getDeathDate() != date("1.1.1")) continue;
		holder.second->setHeir(std::pair(child.second->getID(), child.second));
		return;
	}
}

void CK2::World::resolveTanistry(const std::string& genderLaw, const std::pair<int, std::shared_ptr<Character>>& holder) const
{
	// We have no clue who a tanistry successor might be.
	// Such luck! It's the uncle/aunt the son/daughter was named after!
	resolvePrimogeniture(genderLaw, holder);
	const auto& heir = holder.second->getHeir();
	if (heir.first) heir.second->addYears(35);
}

void CK2::World::resolvePrimogeniture(const std::string& genderLaw, const std::pair<int, std::shared_ptr<Character>>& holder) const
{
	auto children = holder.second->getChildren();

	// Using the awesome knowledge that a smaller ID means earlier character, we don't have to sort them by age.
	std::vector<std::pair<int, std::shared_ptr<Character>>> childVector;
	for (const auto& child: children)
		childVector.emplace_back(child);
	std::sort(childVector.begin(), childVector.end());

	std::pair<int, std::shared_ptr<Character>> son;		  // primary heir candidate
	std::pair<int, std::shared_ptr<Character>> daughter; // primary heir candidate

	for (const auto& child: childVector) {
		if (child.second->getDeathDate() != date("1.1.1")) continue; // Dead.
		if (!son.first && !child.second->isFemale()) son = child;
		if (son.first && !child.second->isFemale() && son.first == child.first - 1) son = child; // Ask paradox. Seriously.
		if (!daughter.first && child.second->isFemale()) daughter = child;
		if (daughter.first && child.second->isFemale() && daughter.first == child.first - 1) daughter = child; // Twins have reversed IDs, yay!
	}

	if ((genderLaw == "agnatic" || genderLaw == "cognatic") && son.first) {
		holder.second->setHeir(son);
		return;
	}
	if (genderLaw == "cognatic" && daughter.first) {
		holder.second->setHeir(daughter);
		return;
	}
	if (genderLaw == "true_cognatic" && (son.first || daughter.first)) {
		if (son.first && daughter.first) {
			if (son.first < daughter.first)
				holder.second->setHeir(son);
			else
				holder.second->setHeir(daughter);

			// Stop! Sanity police!
			if (son.first == daughter.first - 1) holder.second->setHeir(daughter);
			if (daughter.first == son.first - 1) holder.second->setHeir(son);
			// You are insane, you may proceed.

		} else if (son.first)
			holder.second->setHeir(son);
		else
			holder.second->setHeir(daughter);
	}
}

void CK2::World::resolveUltimogeniture(const std::string& genderLaw, const std::pair<int, std::shared_ptr<Character>>& holder) const
{
	auto children = holder.second->getChildren();
	std::vector<std::pair<int, std::shared_ptr<Character>>> childVector;
	for (const auto& child: children)
		childVector.emplace_back(child);
	std::sort(childVector.rbegin(), childVector.rend());
	std::pair<int, std::shared_ptr<Character>> son;
	std::pair<int, std::shared_ptr<Character>> daughter;
	for (const auto& child: childVector) {
		if (child.second->getDeathDate() != date("1.1.1")) continue;
		if (!son.first && !child.second->isFemale()) son = child;
		if (!daughter.first && child.second->isFemale()) daughter = child;
	}

	if ((genderLaw == "agnatic" || genderLaw == "cognatic") && son.first) {
		holder.second->setHeir(son);
		return;
	}
	if (genderLaw == "cognatic" && daughter.first) {
		holder.second->setHeir(daughter);
		return;
	}
	if (genderLaw == "true_cognatic" && (son.first || daughter.first)) {
		if (son.first && daughter.first) {
			if (son.first < daughter.first)
				holder.second->setHeir(son);
			else
				holder.second->setHeir(daughter);
		} else if (son.first)
			holder.second->setHeir(son);
		else
			holder.second->setHeir(daughter);
	}
}


void CK2::World::gatherCourtierNames()
{
	// We're using this function to Locate courtiers, assemble their names as potential Monarch Names in EU4,
	// and also while at it, to see if they hold adviser jobs.

	auto counter = 0;
	auto counterAdvisors = 0;
	std::map<int, std::map<std::string, bool>> holderCourtiers;					 // holder-name/male
	std::map<int, std::map<int, std::shared_ptr<Character>>> holderAdvisors; // holder-advisors

	for (const auto& character: characters.getCharacters()) {
		if (character.second->getHost()) {
			holderCourtiers[character.second->getHost()].insert(std::pair(character.second->getName(), !character.second->isFemale()));
			if (!character.second->getJob().empty()) holderAdvisors[character.second->getHost()].insert(character);
		}
	}
	for (const auto& title: independentTitles) {
		if (title.second->getHolder().first) {
			const auto containerItr = holderCourtiers.find(title.second->getHolder().first);
			if (containerItr != holderCourtiers.end()) {
				title.second->getHolder().second->setCourtierNames(containerItr->second);
				counter += containerItr->second.size();
			}
			const auto adviserItr = holderAdvisors.find(title.second->getHolder().first);
			if (adviserItr != holderAdvisors.end()) {
				title.second->getHolder().second->setAdvisers(adviserItr->second);
				counterAdvisors += adviserItr->second.size();
			}
		}
	}
	Log(LogLevel::Info) << "<> " << counter << " people gathered for interrogation. " << counterAdvisors << " were detained.";
}

void CK2::World::splitVassals()
{
	std::map<std::string, std::shared_ptr<Title>> newIndeps;

	// We have linked counties to provinces, and we know who's independent.
	// We can now go through all titles and see what should be an independent vassal.
	for (const auto& title: independentTitles) {
		if (title.first == "k_papal_state" || title.first == "e_outremer" || title.first == "e_china_west_governor") continue; // Not touching these.
		// let's not split hordes or tribals.
		if (title.second->getHolder().second->getGovernment() == "tribal_government" || title.second->getHolder().second->getGovernment() == "nomadic_government") continue;
		auto relevantVassals = 0;
		std::string relevantVassalPrefix;
		if (title.first.find("e_") == 0)
			relevantVassalPrefix = "k_";
		else if (title.first.find("k_") == 0)
			relevantVassalPrefix = "d_";
		else
			continue; // Not splitting off counties.
		for (const auto& vassal: title.second->getVassals()) {
			if (vassal.first.find(relevantVassalPrefix) != 0) continue; // they are not relevant
			if (vassal.second->coalesceProvinces().empty()) continue;	// no land, not relevant
			relevantVassals++;
		}
		if (!relevantVassals) continue;												// no need to split off anything.
		const auto& provincesClaimed = title.second->coalesceProvinces(); // this is our primary total.
		for (const auto& vassal: title.second->getVassals()) {
			if (vassal.first.find(relevantVassalPrefix) != 0) continue;								  // they are not relevant
			if (vassal.second->getHolder().first == title.second->getHolder().first) continue; // Not splitting our own land.
			const auto& vassalProvincesClaimed = vassal.second->coalesceProvinces();

			// a vassal goes indep if they control 1/relevantvassals + 10% land.
			const double threshold = static_cast<double>(provincesClaimed.size()) / relevantVassals + 0.1 * provincesClaimed.size();
			if (vassalProvincesClaimed.size() > threshold) newIndeps.insert(vassal);
		}
	}

	// Now let's free them.
	for (const auto& newIndep: newIndeps) {
		const auto& liege = newIndep.second->getLiege().second->getTitle();
		liege.second->registerGeneratedVassal(newIndep);
		newIndep.second->clearLiege();
		newIndep.second->registerGeneratedLiege(liege);
		independentTitles.insert(newIndep);
	}
	Log(LogLevel::Info) << "<> " << newIndeps.size() << " vassals liberated from immediate integration.";
}


void CK2::World::verifySave(const std::string& saveGamePath)
{
	std::ifstream saveFile(fs::u8path(saveGamePath));
	if (!saveFile.is_open()) throw std::runtime_error("Could not open save! Exiting!");

	char buffer[3];
	saveFile.get(buffer, 3);
	if (buffer[0] == 'P' && buffer[1] == 'K') {
		if (!uncompressSave(saveGamePath)) throw std::runtime_error("Failed to unpack the compressed save!");
		saveGame.compressed = true;
	}
	saveFile.close();
}

bool CK2::World::uncompressSave(const std::string& saveGamePath)
{
	auto savefile = ZipFile::Open(saveGamePath);
	if (!savefile) return false;
	for (size_t entryNum = 0; entryNum < savefile->GetEntriesCount(); ++entryNum) {
		const auto& entry = savefile->GetEntry(entryNum);
		const auto& name = entry->GetName();
		if (name == "meta") {
			LOG(LogLevel::Info) << ">> Uncompressing metadata";
			saveGame.metadata = std::string{std::istreambuf_iterator<char>(*entry->GetDecompressionStream()), std::istreambuf_iterator<char>()};
		} else if (name == trimPath(saveGamePath)) {
			LOG(LogLevel::Info) << ">> Uncompressing gamestate";
			saveGame.gamestate = std::string{std::istreambuf_iterator<char>(*entry->GetDecompressionStream()), std::istreambuf_iterator<char>()};
		} else
			throw std::runtime_error("Unrecognized savegame structure!");
	}
	return true;
}

void CK2::World::filterIndependentTitles()
{
	const auto& allTitles = titles.getTitles();
	std::map<std::string, std::shared_ptr<Title>> potentialIndeps;

	for (const auto& title: allTitles) {
		const auto& liege = title.second->getLiege();
		const auto& holder = title.second->getHolder();
		if (!holder.first) continue; // don't bother with titles without holders.
		if (liege.first.empty()) {
			// this is a potential indep.
			potentialIndeps.insert(std::pair(title.first, title.second));
		}
	}

	// Check if we hold any actual land (c_something). (Only necessary for the holder,
	// no need to recurse, we're just filtering landless titular titles like mercenaries
	// or landless Pope. If a character holds a landless titular title along actual title
	// (like Caliphate), it's not relevant at this stage as he's independent anyway.

	// First, split off all county_title holders into a container.
	std::set<int> countyHolders;
	for (const auto& title: allTitles) {
		if (title.second->getHolder().first && title.second->getName().find("c_") == 0) { countyHolders.insert(title.second->getHolder().first); }
	}

	// Then look at all potential indeps and see if their holders are up there.
	auto counter = 0;
	for (const auto& indep: potentialIndeps) {
		const auto& holderID = indep.second->getHolder().first;
		if (countyHolders.count(holderID)) {
			// this fellow holds a county, so his indep title is an actual title.
			independentTitles.insert(std::pair(indep.first, indep.second));
			counter++;
		}
	}
	Log(LogLevel::Info) << "<> " << counter << " independent titles recognized.";
}

void CK2::World::mergeIndependentBaronies() const
{
	auto counter = 0;
	const auto& allTitles = titles.getTitles();
	for (const auto& title: allTitles) {
		const auto& holder = title.second->getHolder();
		if (!holder.first) continue; // don't bother with titles without holders.
		const auto& liege = title.second->getLiege();
		if (liege.first.empty()) {
			// this is an indep.
			if (title.first.find("b_") == 0) {
				// it's a barony.
				const auto& djLiege = title.second->getDeJureLiege();
				if (djLiege.first.find("c_") == 0) {
					// we're golden.
					title.second->overrideLiege();
					counter++;
				}
			}
		}
	}
	Log(LogLevel::Info) << "<> " << counter << " baronies reassigned.";
}

void CK2::World::congregateProvinces()
{
	auto counter = 0;
	// We're linking all contained province for a title's tree under that title.
	// This will form actual EU4 tag and contained provinces.
	for (const auto& title: independentTitles) {
		title.second->congregateProvinces(independentTitles);
		for (const auto& province: title.second->getProvinces())
			province.second->loadHoldingTitle(std::pair(title.first, title.second));
		counter += title.second->getProvinces().size();
	}
	Log(LogLevel::Info) << "<> " << counter << " provinces held by independents.";
}


void CK2::World::sanityCheckifyProvinces()
{
	// This is a watchdog function intended to complain if multiple independent titles
	// link to a single province.
	std::map<int, std::vector<std::string>> provinceTitlesMap; // we store all holders for every province.
	auto sanity = true;

	for (const auto& indep: independentTitles) {
		const auto& ownedProvinces = indep.second->getProvinces();
		for (const auto& province: ownedProvinces) {
			provinceTitlesMap[province.first].push_back(indep.first);
		}
	}
	// and now, explode.
	for (const auto& entry: provinceTitlesMap) {
		if (entry.second.size() > 1) {
			std::string warning = "Province ID: " + std::to_string(entry.first) + " is owned by: ";
			for (const auto& owner: entry.second) {
				warning += owner + ",";
			}
			Log(LogLevel::Warning) << warning;
			sanity = false;
		}
	}
	if (sanity) Log(LogLevel::Info) << "<> Province sanity check passed, all provinces accounted for.";
	if (!sanity) Log(LogLevel::Warning) << "!! Province sanity check failed! We have excess provinces!";
}

void CK2::World::shatterEmpires(const Configuration& theConfiguration) const
{
	if (theConfiguration.getShatterEmpires() == ConfigurationDetails::SHATTER_EMPIRES::NONE) {
		Log(LogLevel::Info) << ">< Empire shattering disabled by configuration.";
		return;
	}

	bool shatterKingdoms;
	switch (theConfiguration.getShatterLevel()) {
		case ConfigurationDetails::SHATTER_LEVEL::KINGDOM: shatterKingdoms = false; break;
		default: shatterKingdoms = true;
	}
	const auto& allTitles = titles.getTitles();

	for (const auto& empire: allTitles) {
		if (empire.first.find("e_") != 0) continue;			// Not an empire.
		if (empire.second->getVassals().empty()) continue; // Not relevant.

		// First we are composing a list of all members.
		std::map<std::string, std::shared_ptr<Title>> members;
		for (const auto& vassal: empire.second->getVassals()) {
			if (vassal.first.find("d_") == 0 || vassal.first.find("c_") == 0) {
				members.insert(std::pair(vassal.first, vassal.second));
			} else if (vassal.first.find("k_") == 0) {
				if (shatterKingdoms && vassal.first != "k_papal_state" && vassal.first != "k_orthodox") { // hard override for special empire members
					for (const auto& vassalvassal: vassal.second->getVassals()) {
						members.insert(std::pair(vassalvassal.first, vassalvassal.second));
					}
					// Bricking the kingdom
					vassal.second->clearVassals();
					vassal.second->clearHolder();
					vassal.second->clearLiege();
				} else {
					// Not shattering kingdoms.
					members.insert(std::pair(vassal.first, vassal.second));
				}
			} else {
				Log(LogLevel::Warning) << "Unrecognized vassal level: " << vassal.first;
			}
		}

		for (const auto& member: members) {
			member.second->clearLiege();
		}

		// Finally we are clearing empire's vassal links, leaving it standalone.
		empire.second->clearVassals();
		empire.second->clearHolder();
		Log(LogLevel::Info) << "<> " << empire.first << " shattered, " << members.size() << " members released.";
	}
}

void CK2::World::shatterHRE(const Configuration& theConfiguration) const
{
	if (theConfiguration.getHRE() == ConfigurationDetails::I_AM_HRE::NONE) {
		Log(LogLevel::Info) << ">< HRE Mechanics and shattering overridden by configuration.";
		return;
	}

	std::string hreTitle;
	switch (theConfiguration.getHRE()) {
		case ConfigurationDetails::I_AM_HRE::HRE: hreTitle = "e_hre"; break;
		case ConfigurationDetails::I_AM_HRE::BYZANTIUM: hreTitle = "e_byzantium"; break;
		case ConfigurationDetails::I_AM_HRE::ROME: hreTitle = "e_roman_empire"; break;
		case ConfigurationDetails::I_AM_HRE::CUSTOM: hreTitle = iAmHreMapper.getHRE(); break;
		default: hreTitle = "e_hre";
	}
	const auto& allTitles = titles.getTitles();
	const auto& theHre = allTitles.find(hreTitle);
	if (theHre == allTitles.end()) {
		Log(LogLevel::Info) << "><  HRE shattering cancelled, " << hreTitle << " not found!";
		return;
	}
	if (theHre->second->getVassals().empty()) {
		Log(LogLevel::Info) << "><  HRE shattering cancelled, " << hreTitle << " has no vassals!";
		return;
	}
	const auto& hreHolder = theHre->second->getHolder();
	bool emperorSet = false;

	// First we are composing a list of all HRE members. These are duchies,
	// so we're also ripping them from under any potential kingdoms.
	std::map<std::string, std::shared_ptr<Title>> hreMembers;
	for (const auto& vassal: theHre->second->getVassals()) {
		if (vassal.first.find("d_") == 0 || vassal.first.find("c_") == 0) {
			hreMembers.insert(std::pair(vassal.first, vassal.second));
		} else if (vassal.first.find("k_") == 0) {
			if (vassal.first == "k_papal_state" || vassal.first == "k_orthodox") // hard override for special HRE members
			{
				hreMembers.insert(std::pair(vassal.first, vassal.second));
				continue;
			}
			for (const auto& vassalvassal: vassal.second->getVassals()) {
				hreMembers.insert(std::pair(vassalvassal.first, vassalvassal.second));
			}
			// Bricking the kingdom.
			vassal.second->clearVassals();
			vassal.second->clearHolder();
			vassal.second->clearLiege();
		} else if (vassal.first.find("b_") != 0) {
			Log(LogLevel::Warning) << "Unrecognized HRE vassal: " << vassal.first;
		}
	}

	for (const auto& member: hreMembers) {
		// We're flagging hre members as such, as well as setting them free.
		// We're also on the lookout on the current HRE emperor.
		if (!emperorSet && member.second->getHolder().first == hreHolder.first) {
			// This is the emperor. He may hold several duchies, but the first one
			// we find will be flagged emperor.
			member.second->setHREEmperor();
			emperorSet = true;
		}
		member.second->setInHRE();
		member.second->clearLiege();
	}

	// Finally we are clearing hreTitle's vassal links, leaving it standalone.
	theHre->second->clearVassals();
	theHre->second->clearHolder();
	Log(LogLevel::Info) << "<> " << hreMembers.size() << " HRE members released.";
}

void CK2::World::filterProvincelessTitles()
{
	auto counter = 0;
	std::set<std::string> titlesForDisposal;
	for (const auto& title: independentTitles) {
		if (title.second->getProvinces().empty()) titlesForDisposal.insert(title.first);
	}
	for (const auto& drop: titlesForDisposal) {
		independentTitles.erase(drop);
		counter++;
	}
	Log(LogLevel::Info) << "<> " << counter << " empty titles dropped, " << independentTitles.size() << " remain.";
}
