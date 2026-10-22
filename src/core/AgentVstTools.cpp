/*
 * AgentVstTools.cpp - Native LMMS Agent Tools for VST Synthesizers & Audio Effects
 *
 * Provides full AI discovery, loading, sound design knob inspection, parameter automation,
 * MIDI CC expression control, preset bank management, GUI toggling, and effect bypassing.
 *
 * Uses LMMS core RemotePlugin IPC and Model child reflection without altering base Plugin vtable.
 */

#include "AgentVstTools.h"
#include "AgentToolRegistry.h"

#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <algorithm>

#include "AudioEngine.h"
#include "AutomatableModel.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Effect.h"
#include "EffectChain.h"
#include "EffectControls.h"
#include "Engine.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "MidiEvent.h"
#include "Mixer.h"
#include "PatternStore.h"
#include "PluginFactory.h"
#include "RemotePlugin.h"
#include "Song.h"
#include "TimePos.h"
#include "plugins/VstBase/communication.h"

namespace lmms
{

// -----------------------------------------------------------------------
// Helper: Resolve target plugin (Instrument or Effect)
// -----------------------------------------------------------------------
static Plugin* resolveTargetPlugin(const QJsonObject& args, Song* song, Mixer* mixer, QString& errorMsg)
{
	if (args.contains(QStringLiteral("track_index")))
	{
		int trackIdx = args.value(QStringLiteral("track_index")).toInt(-1);
		if (trackIdx < 0 || trackIdx >= static_cast<int>(song->tracks().size()))
		{
			errorMsg = QStringLiteral("track_index %1 is out of range (song has %2 tracks).")
				.arg(trackIdx).arg(song->tracks().size());
			return nullptr;
		}
		auto it = dynamic_cast<InstrumentTrack*>(song->tracks()[trackIdx]);
		if (!it || !it->instrument())
		{
			errorMsg = QStringLiteral("Track %1 ('%2') is not an InstrumentTrack or has no instrument loaded.")
				.arg(trackIdx).arg(song->tracks()[trackIdx]->name());
			return nullptr;
		}
		return it->instrument();
	}
	else if (args.contains(QStringLiteral("channel_index")) && args.contains(QStringLiteral("effect_index")))
	{
		int chIdx = args.value(QStringLiteral("channel_index")).toInt(-1);
		int effIdx = args.value(QStringLiteral("effect_index")).toInt(-1);
		if (chIdx < 0 || chIdx >= static_cast<int>(mixer->numChannels()))
		{
			errorMsg = QStringLiteral("channel_index %1 is out of range (mixer has %2 channels).")
				.arg(chIdx).arg(mixer->numChannels());
			return nullptr;
		}
		MixerChannel* ch = mixer->mixerChannel(chIdx);
		if (!ch)
		{
			errorMsg = QStringLiteral("Could not retrieve mixer channel %1.").arg(chIdx);
			return nullptr;
		}
		const auto& effects = ch->m_fxChain.effects();
		if (effIdx < 0 || effIdx >= static_cast<int>(effects.size()))
		{
			errorMsg = QStringLiteral("effect_index %1 is out of range for channel %2 ('%3', has %4 effects).")
				.arg(effIdx).arg(chIdx).arg(ch->m_name).arg(effects.size());
			return nullptr;
		}
		return effects[effIdx];
	}

	errorMsg = QStringLiteral("You must specify either 'track_index' (for synths) or both 'channel_index' and 'effect_index' (for mixer effects).");
	return nullptr;
}

// -----------------------------------------------------------------------
// Helper: Resolve RemotePlugin associated with a VST plugin
// -----------------------------------------------------------------------
static RemotePlugin* resolveTargetRemotePlugin(Plugin* plugin)
{
	if (!plugin) return nullptr;

	RemotePlugin* rp = plugin->findChild<RemotePlugin*>();
	if (rp) return rp;

	if (auto eff = dynamic_cast<Effect*>(plugin))
	{
		if (eff->controls())
		{
			rp = eff->controls()->findChild<RemotePlugin*>();
			if (rp) return rp;
		}
	}
	return nullptr;
}

// -----------------------------------------------------------------------
// Helper: Find child FloatModel by parameter index
// -----------------------------------------------------------------------
static FloatModel* findParamModel(Plugin* plugin, int paramIdx)
{
	if (!plugin) return nullptr;

	QString targetName = QString::number(paramIdx);
	QList<FloatModel*> models = plugin->findChildren<FloatModel*>();
	for (FloatModel* m : models)
	{
		if (m && m->displayName() == targetName)
		{
			return m;
		}
	}

	if (auto eff = dynamic_cast<Effect*>(plugin))
	{
		if (eff->controls())
		{
			QList<FloatModel*> ctlModels = eff->controls()->findChildren<FloatModel*>();
			for (FloatModel* m : ctlModels)
			{
				if (m && m->displayName() == targetName)
				{
					return m;
				}
			}
		}
	}
	return nullptr;
}

// -----------------------------------------------------------------------
// Helper: Recursive filesystem scanner for VST files
// -----------------------------------------------------------------------
static void scanDirectoryForVst(const QString& dirPath, int maxDepth, int currentDepth, QJsonArray& pluginList, QSet<QString>& seenPaths)
{
	if (currentDepth > maxDepth) return;
	QDir dir(dirPath);
	if (!dir.exists()) return;

	QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name);
	for (const QFileInfo& fi : entries)
	{
		QString absPath = fi.absoluteFilePath();
		QString normPath = QDir::toNativeSeparators(absPath);
		QString suffix = fi.suffix().toLower();

		if (fi.isDir())
		{
			if (suffix == QStringLiteral("vst3"))
			{
				if (!seenPaths.contains(normPath))
				{
					seenPaths.insert(normPath);
					QJsonObject pObj;
					pObj[QStringLiteral("name")] = fi.baseName();
					pObj[QStringLiteral("format")] = QStringLiteral("vst3");
					pObj[QStringLiteral("path")] = normPath;

					QString lowerName = fi.baseName().toLower();
					if (lowerName.contains(QStringLiteral("synth")) || lowerName.contains(QStringLiteral("lead")) ||
					    lowerName.contains(QStringLiteral("bass")) || lowerName.contains(QStringLiteral("piano")) ||
					    lowerName.contains(QStringLiteral("vital")) || lowerName.contains(QStringLiteral("surge")) ||
					    lowerName.contains(QStringLiteral("drum")) || lowerName.contains(QStringLiteral("keys")))
					{
						pObj[QStringLiteral("type")] = QStringLiteral("instrument");
					}
					else if (lowerName.contains(QStringLiteral("fx")) || lowerName.contains(QStringLiteral("delay")) ||
					         lowerName.contains(QStringLiteral("reverb")) || lowerName.contains(QStringLiteral("comp")) ||
					         lowerName.contains(QStringLiteral("crush")) || lowerName.contains(QStringLiteral("distort")) ||
					         lowerName.contains(QStringLiteral("filter")) || lowerName.contains(QStringLiteral("eq")))
					{
						pObj[QStringLiteral("type")] = QStringLiteral("effect");
					}
					else
					{
						pObj[QStringLiteral("type")] = QStringLiteral("unknown");
					}
					pluginList.append(pObj);
				}
			}
			else
			{
				scanDirectoryForVst(absPath, maxDepth, currentDepth + 1, pluginList, seenPaths);
			}
		}
		else if (fi.isFile())
		{
			if (suffix == QStringLiteral("dll") || suffix == QStringLiteral("vst3"))
			{
				if (!seenPaths.contains(normPath))
				{
					seenPaths.insert(normPath);
					QJsonObject pObj;
					pObj[QStringLiteral("name")] = fi.baseName();
					pObj[QStringLiteral("format")] = (suffix == QStringLiteral("vst3")) ? QStringLiteral("vst3") : QStringLiteral("vst2");
					pObj[QStringLiteral("path")] = normPath;

					QString lowerName = fi.baseName().toLower();
					if (lowerName.contains(QStringLiteral("synth")) || lowerName.contains(QStringLiteral("lead")) ||
					    lowerName.contains(QStringLiteral("bass")) || lowerName.contains(QStringLiteral("piano")) ||
					    lowerName.contains(QStringLiteral("vital")) || lowerName.contains(QStringLiteral("surge")) ||
					    lowerName.contains(QStringLiteral("drum")) || lowerName.contains(QStringLiteral("keys")))
					{
						pObj[QStringLiteral("type")] = QStringLiteral("instrument");
					}
					else if (lowerName.contains(QStringLiteral("fx")) || lowerName.contains(QStringLiteral("delay")) ||
					         lowerName.contains(QStringLiteral("reverb")) || lowerName.contains(QStringLiteral("comp")) ||
					         lowerName.contains(QStringLiteral("crush")) || lowerName.contains(QStringLiteral("distort")) ||
					         lowerName.contains(QStringLiteral("filter")) || lowerName.contains(QStringLiteral("eq")))
					{
						pObj[QStringLiteral("type")] = QStringLiteral("effect");
					}
					else
					{
						pObj[QStringLiteral("type")] = QStringLiteral("unknown");
					}
					pluginList.append(pObj);
				}
			}
		}
	}
}

// -----------------------------------------------------------------------
// 1. scan_vst_plugins
// -----------------------------------------------------------------------
static QJsonObject handleScanVstPlugins(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Q_UNUSED(errorMsg);

	QStringList searchDirs;

	QString userVst = ConfigManager::inst()->vstDir();
	if (!userVst.isEmpty() && QDir(userVst).exists())
	{
		searchDirs.append(userVst);
	}

	searchDirs.append(QStringLiteral("C:/Program Files/Common Files/VST3"));
	searchDirs.append(QStringLiteral("C:/Program Files/Common Files/VST2"));
	searchDirs.append(QStringLiteral("C:/Program Files/VSTPlugins"));
	searchDirs.append(QStringLiteral("C:/Program Files/Steinberg/VSTPlugins"));
	searchDirs.append(QStringLiteral("C:/Program Files (x86)/Common Files/VST3"));
	searchDirs.append(QStringLiteral("C:/Program Files (x86)/VSTPlugins"));
	searchDirs.append(QStringLiteral("C:/Program Files (x86)/Steinberg/VSTPlugins"));
	searchDirs.append(QStringLiteral("C:/VstPlugins"));

	if (args.contains(QStringLiteral("custom_path")))
	{
		QString custom = args.value(QStringLiteral("custom_path")).toString().trimmed();
		if (!custom.isEmpty() && QDir(custom).exists())
		{
			searchDirs.prepend(custom);
		}
	}

	QJsonArray pluginList;
	QSet<QString> seenPaths;

	for (const QString& sDir : searchDirs)
	{
		if (QDir(sDir).exists())
		{
			scanDirectoryForVst(sDir, 5, 0, pluginList, seenPaths);
		}
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("VST plugin scan completed successfully.");
	data[QStringLiteral("count")] = pluginList.size();
	data[QStringLiteral("plugins")] = pluginList;
	return data;
}

// -----------------------------------------------------------------------
// 2. load_vst_instrument
// -----------------------------------------------------------------------
static QJsonObject handleLoadVstInstrument(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	if (!args.contains(QStringLiteral("plugin_path")))
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameter: 'plugin_path'.");
		return QJsonObject();
	}

	QString pluginPath = args.value(QStringLiteral("plugin_path")).toString().trimmed();
	if (pluginPath.isEmpty())
	{
		ok = false;
		errorMsg = QStringLiteral("Parameter 'plugin_path' cannot be empty.");
		return QJsonObject();
	}

	QFileInfo fi(pluginPath);
	if (!fi.exists())
	{
		QString fallback = ConfigManager::inst()->vstDir() + QDir::separator() + pluginPath;
		if (QFileInfo(fallback).exists())
		{
			pluginPath = fallback;
			fi.setFile(pluginPath);
		}
		else
		{
			ok = false;
			errorMsg = QStringLiteral("VST plugin file '%1' was not found on disk.").arg(pluginPath);
			return QJsonObject();
		}
	}

	bool inBBEditor = args.value(QStringLiteral("in_bb_editor")).toBool(false);
	TrackContainer* tc = (inBBEditor && Engine::patternStore()) ? static_cast<TrackContainer*>(Engine::patternStore()) : static_cast<TrackContainer*>(song);

	InstrumentTrack* it = nullptr;
	int targetTrackIdx = -1;

	if (args.contains(QStringLiteral("track_index")))
	{
		int reqIdx = args.value(QStringLiteral("track_index")).toInt();
		const auto& tracks = tc->tracks();
		if (reqIdx >= 0 && reqIdx < static_cast<int>(tracks.size()))
		{
			it = dynamic_cast<InstrumentTrack*>(tracks[reqIdx]);
			if (it)
			{
				targetTrackIdx = reqIdx;
			}
		}
	}

	if (!it)
	{
		Track* t = Track::create(Track::Type::Instrument, tc);
		it = dynamic_cast<InstrumentTrack*>(t);
		if (!it)
		{
			ok = false;
			errorMsg = QStringLiteral("Failed to create InstrumentTrack.");
			return QJsonObject();
		}
		const auto& tracks = tc->tracks();
		for (size_t i = 0; i < tracks.size(); ++i)
		{
			if (tracks[i] == it)
			{
				targetTrackIdx = static_cast<int>(i);
				break;
			}
		}
	}

	QString hostName = QStringLiteral("vestige");
	Instrument* inst = it->loadInstrument(hostName);
	if (!inst)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to instantiate host instrument plugin '%1'.").arg(hostName);
		return QJsonObject();
	}

	inst->loadFile(pluginPath);

	if (args.contains(QStringLiteral("name")) && !args.value(QStringLiteral("name")).toString().trimmed().isEmpty())
	{
		it->setName(args.value(QStringLiteral("name")).toString().trimmed());
	}
	else
	{
		it->setName(fi.baseName());
	}

	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("VST instrument loaded successfully.");
	data[QStringLiteral("track_index")] = targetTrackIdx;
	data[QStringLiteral("track_name")] = it->name();
	data[QStringLiteral("host_engine")] = hostName;
	data[QStringLiteral("plugin_path")] = QDir::toNativeSeparators(pluginPath);
	data[QStringLiteral("in_bb_editor")] = inBBEditor;
	return data;
}

// -----------------------------------------------------------------------
// 3. add_vst_effect
// -----------------------------------------------------------------------
static QJsonObject handleAddVstEffect(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Mixer* mixer = Engine::mixer();
	Song* song = Engine::getSong();
	if (!mixer || !song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Mixer or Song engine is not available.");
		return QJsonObject();
	}

	if (!args.contains(QStringLiteral("channel_index")) || !args.contains(QStringLiteral("plugin_path")))
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameters: 'channel_index' and 'plugin_path'.");
		return QJsonObject();
	}

	int channelIdx = args.value(QStringLiteral("channel_index")).toInt(-1);
	if (channelIdx < 0 || channelIdx >= static_cast<int>(mixer->numChannels()))
	{
		ok = false;
		errorMsg = QStringLiteral("Invalid channel_index: %1. Total mixer channels: %2.").arg(channelIdx).arg(mixer->numChannels());
		return QJsonObject();
	}

	MixerChannel* ch = mixer->mixerChannel(channelIdx);
	if (!ch)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to retrieve mixer channel %1.").arg(channelIdx);
		return QJsonObject();
	}

	QString pluginPath = args.value(QStringLiteral("plugin_path")).toString().trimmed();
	QFileInfo fi(pluginPath);
	if (!fi.exists())
	{
		QString fallback = ConfigManager::inst()->vstDir() + QDir::separator() + pluginPath;
		if (QFileInfo(fallback).exists())
		{
			pluginPath = fallback;
			fi.setFile(pluginPath);
		}
		else
		{
			ok = false;
			errorMsg = QStringLiteral("VST effect plugin file '%1' was not found on disk.").arg(pluginPath);
			return QJsonObject();
		}
	}

	Plugin::Descriptor::SubPluginFeatures::Key key;
	key.attributes[QStringLiteral("file")] = pluginPath;
	key.name = fi.baseName();

	Effect* eff = Effect::instantiate(QStringLiteral("vsteffect"), &ch->m_fxChain, &key);
	if (!eff || !eff->isOkay())
	{
		delete eff;
		ok = false;
		errorMsg = QStringLiteral("Failed to instantiate VST effect for '%1'.").arg(pluginPath);
		return QJsonObject();
	}

	if (args.contains(QStringLiteral("wet_dry")))
	{
		float wd = static_cast<float>(args.value(QStringLiteral("wet_dry")).toDouble(1.0));
		eff->wetDryModel()->setValue(std::clamp(wd, 0.0f, 1.0f));
	}

	if (args.contains(QStringLiteral("enabled")))
	{
		eff->enabledModel()->setValue(args.value(QStringLiteral("enabled")).toBool(true));
	}

	ch->m_fxChain.appendEffect(eff);
	song->setModified();

	int effIndex = static_cast<int>(ch->m_fxChain.effects().size()) - 1;

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("VST effect inserted onto mixer rack successfully.");
	data[QStringLiteral("channel_index")] = channelIdx;
	data[QStringLiteral("effect_index")] = effIndex;
	data[QStringLiteral("effect_name")] = eff->displayName();
	data[QStringLiteral("plugin_path")] = QDir::toNativeSeparators(pluginPath);
	data[QStringLiteral("wet_dry")] = eff->wetDryModel()->value();
	data[QStringLiteral("enabled")] = eff->enabledModel()->value();
	return data;
}

// -----------------------------------------------------------------------
// 4. get_vst_parameters
// -----------------------------------------------------------------------
static QJsonObject handleGetVstParameters(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	Mixer* mixer = Engine::mixer();
	if (!song || !mixer)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song or Mixer engine is not available.");
		return QJsonObject();
	}

	Plugin* plugin = resolveTargetPlugin(args, song, mixer, errorMsg);
	if (!plugin)
	{
		ok = false;
		return QJsonObject();
	}

	QList<FloatModel*> floatModels = plugin->findChildren<FloatModel*>();
	if (auto eff = dynamic_cast<Effect*>(plugin))
	{
		if (eff->controls())
		{
			floatModels.append(eff->controls()->findChildren<FloatModel*>());
		}
	}

	QMap<int, FloatModel*> indexToModel;
	for (FloatModel* m : floatModels)
	{
		if (!m) continue;
		bool isInt = false;
		int idx = m->displayName().toInt(&isInt);
		if (isInt && idx >= 0)
		{
			indexToModel[idx] = m;
		}
	}

	int totalCount = indexToModel.isEmpty() ? 0 : (indexToModel.lastKey() + 1);
	QString query = args.value(QStringLiteral("query")).toString().trimmed();
	QJsonArray paramArray;

	for (int i = 0; i < totalCount; ++i)
	{
		QString pName = QStringLiteral("Param %1").arg(i);
		float val = 0.0f;
		if (indexToModel.contains(i) && indexToModel[i])
		{
			val = indexToModel[i]->value();
		}

		if (!query.isEmpty())
		{
			bool match = pName.contains(query, Qt::CaseInsensitive) ||
			             QString::number(i) == query;
			if (!match) continue;
		}

		QJsonObject pObj;
		pObj[QStringLiteral("index")] = i;
		pObj[QStringLiteral("name")] = pName;
		pObj[QStringLiteral("value")] = val;
		pObj[QStringLiteral("display")] = QString::number(val, 'f', 2);
		paramArray.append(pObj);
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("plugin_name")] = plugin->displayName();
	data[QStringLiteral("total_param_count")] = totalCount;
	data[QStringLiteral("matched_count")] = paramArray.size();
	data[QStringLiteral("parameters")] = paramArray;
	return data;
}

// -----------------------------------------------------------------------
// 5. set_vst_parameter
// -----------------------------------------------------------------------
static QJsonObject handleSetVstParameter(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	Mixer* mixer = Engine::mixer();
	if (!song || !mixer)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song or Mixer engine is not available.");
		return QJsonObject();
	}

	if (!args.contains(QStringLiteral("param_index")) || !args.contains(QStringLiteral("value")))
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameters: 'param_index' and 'value'.");
		return QJsonObject();
	}

	int paramIdx = args.value(QStringLiteral("param_index")).toInt(-1);
	float val = static_cast<float>(args.value(QStringLiteral("value")).toDouble());
	val = std::clamp(val, 0.0f, 1.0f);

	Plugin* plugin = resolveTargetPlugin(args, song, mixer, errorMsg);
	if (!plugin)
	{
		ok = false;
		return QJsonObject();
	}

	FloatModel* m = findParamModel(plugin, paramIdx);
	if (!m)
	{
		ok = false;
		errorMsg = QStringLiteral("Could not find parameter model %1 on plugin '%2'.").arg(paramIdx).arg(plugin->displayName());
		return QJsonObject();
	}

	m->setValue(val);
	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("VST parameter tweaked successfully.");
	data[QStringLiteral("plugin_name")] = plugin->displayName();
	data[QStringLiteral("param_index")] = paramIdx;
	data[QStringLiteral("value")] = val;
	return data;
}

// -----------------------------------------------------------------------
// 6. select_vst_program
// -----------------------------------------------------------------------
static QJsonObject handleSelectVstProgram(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	Mixer* mixer = Engine::mixer();
	if (!song || !mixer)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song or Mixer engine is not available.");
		return QJsonObject();
	}

	Plugin* plugin = resolveTargetPlugin(args, song, mixer, errorMsg);
	if (!plugin)
	{
		ok = false;
		return QJsonObject();
	}

	RemotePlugin* rp = resolveTargetRemotePlugin(plugin);
	if (!rp)
	{
		ok = false;
		errorMsg = QStringLiteral("Plugin '%1' does not expose a remote VST engine.").arg(plugin->displayName());
		return QJsonObject();
	}

	int programIdx = args.value(QStringLiteral("program_index")).toInt(0);

	rp->lock();
	rp->sendMessage(RemotePlugin::message(IdVstSetProgram).addInt(programIdx));
	rp->waitForMessage(IdVstSetProgram, true);
	rp->unlock();

	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("VST program preset selected successfully.");
	data[QStringLiteral("plugin_name")] = plugin->displayName();
	data[QStringLiteral("program_index")] = programIdx;
	return data;
}

// -----------------------------------------------------------------------
// 7. load_vst_preset_file
// -----------------------------------------------------------------------
static QJsonObject handleLoadVstPresetFile(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	Mixer* mixer = Engine::mixer();
	if (!song || !mixer)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song or Mixer engine is not available.");
		return QJsonObject();
	}

	if (!args.contains(QStringLiteral("file_path")))
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameter: 'file_path'.");
		return QJsonObject();
	}

	QString filePath = args.value(QStringLiteral("file_path")).toString().trimmed();
	if (!QFileInfo::exists(filePath))
	{
		ok = false;
		errorMsg = QStringLiteral("Preset file '%1' was not found on disk.").arg(filePath);
		return QJsonObject();
	}

	Plugin* plugin = resolveTargetPlugin(args, song, mixer, errorMsg);
	if (!plugin)
	{
		ok = false;
		return QJsonObject();
	}

	RemotePlugin* rp = resolveTargetRemotePlugin(plugin);
	if (!rp)
	{
		ok = false;
		errorMsg = QStringLiteral("Plugin '%1' does not expose a remote VST engine.").arg(plugin->displayName());
		return QJsonObject();
	}

	rp->lock();
	rp->sendMessage(RemotePlugin::message(IdLoadPresetFile).addString(QSTR_TO_STDSTR(QDir::toNativeSeparators(filePath))));
	rp->waitForMessage(IdLoadPresetFile, true);
	rp->unlock();

	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("VST preset file loaded successfully.");
	data[QStringLiteral("plugin_name")] = plugin->displayName();
	data[QStringLiteral("file_path")] = QDir::toNativeSeparators(filePath);
	return data;
}

// -----------------------------------------------------------------------
// 8. link_vst_to_automation
// -----------------------------------------------------------------------
static QJsonObject handleLinkVstToAutomation(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	Mixer* mixer = Engine::mixer();
	if (!song || !mixer)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song or Mixer engine is not available.");
		return QJsonObject();
	}

	if (!args.contains(QStringLiteral("param_index")) || !args.contains(QStringLiteral("automation_track_index")))
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameters: 'param_index' and 'automation_track_index'.");
		return QJsonObject();
	}

	int paramIdx = args.value(QStringLiteral("param_index")).toInt(-1);
	int autoTrackIdx = args.value(QStringLiteral("automation_track_index")).toInt(-1);
	int clipIdx = args.value(QStringLiteral("clip_index")).toInt(0);

	Plugin* plugin = resolveTargetPlugin(args, song, mixer, errorMsg);
	if (!plugin)
	{
		ok = false;
		return QJsonObject();
	}

	FloatModel* targetModel = findParamModel(plugin, paramIdx);
	if (!targetModel)
	{
		ok = false;
		errorMsg = QStringLiteral("Could not locate AutomatableModel for parameter index %1 on plugin '%2'.")
			.arg(paramIdx).arg(plugin->displayName());
		return QJsonObject();
	}

	const auto& tracks = song->tracks();
	if (autoTrackIdx < 0 || autoTrackIdx >= static_cast<int>(tracks.size()))
	{
		ok = false;
		errorMsg = QStringLiteral("automation_track_index %1 is out of range (song has %2 tracks).")
			.arg(autoTrackIdx).arg(tracks.size());
		return QJsonObject();
	}

	auto at = dynamic_cast<AutomationTrack*>(tracks[autoTrackIdx]);
	if (!at)
	{
		ok = false;
		errorMsg = QStringLiteral("Track %1 is not an AutomationTrack.").arg(autoTrackIdx);
		return QJsonObject();
	}

	AutomationClip* ac = nullptr;
	if (clipIdx >= 0 && clipIdx < at->numOfClips())
	{
		ac = dynamic_cast<AutomationClip*>(at->getClip(clipIdx));
	}
	else
	{
		ac = dynamic_cast<AutomationClip*>(at->createClip(TimePos(0)));
		clipIdx = at->numOfClips() - 1;
	}

	if (!ac)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to access or create AutomationClip on track %1.").arg(autoTrackIdx);
		return QJsonObject();
	}

	ac->addObject(targetModel);
	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("VST parameter linked to automation clip successfully.");
	data[QStringLiteral("plugin_name")] = plugin->displayName();
	data[QStringLiteral("param_index")] = paramIdx;
	data[QStringLiteral("param_name")] = targetModel->displayName();
	data[QStringLiteral("automation_track_index")] = autoTrackIdx;
	data[QStringLiteral("clip_index")] = clipIdx;
	return data;
}

// -----------------------------------------------------------------------
// 9. send_vst_midi_cc
// -----------------------------------------------------------------------
static QJsonObject handleSendVstMidiCC(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	if (!args.contains(QStringLiteral("track_index")) || !args.contains(QStringLiteral("cc_number")) || !args.contains(QStringLiteral("value")))
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameters: 'track_index', 'cc_number', and 'value'.");
		return QJsonObject();
	}

	int trackIdx = args.value(QStringLiteral("track_index")).toInt(-1);
	int ccNumber = args.value(QStringLiteral("cc_number")).toInt(0);
	int val = args.value(QStringLiteral("value")).toInt(0);
	f_cnt_t tickOffset = static_cast<f_cnt_t>(args.value(QStringLiteral("tick_offset")).toInt(0));
	bool isPitchBend = args.value(QStringLiteral("is_pitch_bend")).toBool(false);

	const auto& tracks = song->tracks();
	if (trackIdx < 0 || trackIdx >= static_cast<int>(tracks.size()))
	{
		ok = false;
		errorMsg = QStringLiteral("track_index %1 is out of range.").arg(trackIdx);
		return QJsonObject();
	}

	auto it = dynamic_cast<InstrumentTrack*>(tracks[trackIdx]);
	if (!it || !it->instrument())
	{
		ok = false;
		errorMsg = QStringLiteral("Track %1 is not an InstrumentTrack or has no instrument.").arg(trackIdx);
		return QJsonObject();
	}

	if (isPitchBend || ccNumber == -1 || ccNumber == 128)
	{
		int bendVal = std::clamp(val + 8192, 0, 16383);
		int lsb = bendVal & 0x7F;
		int msb = (bendVal >> 7) & 0x7F;
		MidiEvent ev(MidiPitchBend, 0, lsb, msb);
		it->instrument()->handleMidiEvent(ev, TimePos(), tickOffset);
	}
	else
	{
		int cc = std::clamp(ccNumber, 0, 127);
		int ccVal = std::clamp(val, 0, 127);
		MidiEvent ev(MidiControlChange, 0, cc, ccVal);
		it->instrument()->handleMidiEvent(ev, TimePos(), tickOffset);
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("MIDI CC / pitch event sent to VST successfully.");
	data[QStringLiteral("track_index")] = trackIdx;
	data[QStringLiteral("track_name")] = it->name();
	data[QStringLiteral("cc_number")] = ccNumber;
	data[QStringLiteral("value")] = val;
	data[QStringLiteral("tick_offset")] = static_cast<int>(tickOffset);
	return data;
}

// -----------------------------------------------------------------------
// 10. save_vst_preset
// -----------------------------------------------------------------------
static QJsonObject handleSaveVstPreset(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	Mixer* mixer = Engine::mixer();
	if (!song || !mixer)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song or Mixer engine is not available.");
		return QJsonObject();
	}

	if (!args.contains(QStringLiteral("output_path")))
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameter: 'output_path'.");
		return QJsonObject();
	}

	QString outPath = args.value(QStringLiteral("output_path")).toString().trimmed();
	if (outPath.isEmpty())
	{
		ok = false;
		errorMsg = QStringLiteral("Parameter 'output_path' cannot be empty.");
		return QJsonObject();
	}

	QFileInfo fi(outPath);
	QDir().mkpath(fi.absolutePath());

	Plugin* plugin = resolveTargetPlugin(args, song, mixer, errorMsg);
	if (!plugin)
	{
		ok = false;
		return QJsonObject();
	}

	RemotePlugin* rp = resolveTargetRemotePlugin(plugin);
	if (!rp)
	{
		ok = false;
		errorMsg = QStringLiteral("Plugin '%1' does not expose a remote VST engine.").arg(plugin->displayName());
		return QJsonObject();
	}

	rp->lock();
	rp->sendMessage(RemotePlugin::message(IdSavePresetFile).addString(QSTR_TO_STDSTR(QDir::toNativeSeparators(outPath))));
	rp->waitForMessage(IdSavePresetFile, true);
	rp->unlock();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("VST preset saved to disk successfully.");
	data[QStringLiteral("plugin_name")] = plugin->displayName();
	data[QStringLiteral("output_path")] = QDir::toNativeSeparators(outPath);
	return data;
}

// -----------------------------------------------------------------------
// 11. toggle_vst_gui
// -----------------------------------------------------------------------
static QJsonObject handleToggleVstGui(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	Mixer* mixer = Engine::mixer();
	if (!song || !mixer)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song or Mixer engine is not available.");
		return QJsonObject();
	}

	Plugin* plugin = resolveTargetPlugin(args, song, mixer, errorMsg);
	if (!plugin)
	{
		ok = false;
		return QJsonObject();
	}

	RemotePlugin* rp = resolveTargetRemotePlugin(plugin);
	if (!rp)
	{
		ok = false;
		errorMsg = QStringLiteral("Plugin '%1' does not expose a remote VST editor.").arg(plugin->displayName());
		return QJsonObject();
	}

	if (args.contains(QStringLiteral("visible")))
	{
		bool vis = args.value(QStringLiteral("visible")).toBool();
		if (vis)
		{
			rp->showUI();
		}
		else
		{
			rp->hideUI();
		}
	}
	else
	{
		rp->toggleUI();
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("VST GUI visibility toggled.");
	data[QStringLiteral("plugin_name")] = plugin->displayName();
	data[QStringLiteral("visible")] = (rp->isUIVisible() == 1);
	return data;
}

// -----------------------------------------------------------------------
// 12. bypass_vst
// -----------------------------------------------------------------------
static QJsonObject handleBypassVst(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Mixer* mixer = Engine::mixer();
	Song* song = Engine::getSong();
	if (!mixer || !song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Mixer or Song engine is not available.");
		return QJsonObject();
	}

	if (!args.contains(QStringLiteral("channel_index")) || !args.contains(QStringLiteral("effect_index")))
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameters: 'channel_index' and 'effect_index'.");
		return QJsonObject();
	}

	int chIdx = args.value(QStringLiteral("channel_index")).toInt(-1);
	int effIdx = args.value(QStringLiteral("effect_index")).toInt(-1);

	if (chIdx < 0 || chIdx >= static_cast<int>(mixer->numChannels()))
	{
		ok = false;
		errorMsg = QStringLiteral("Invalid channel_index %1.").arg(chIdx);
		return QJsonObject();
	}

	MixerChannel* ch = mixer->mixerChannel(chIdx);
	if (!ch)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to retrieve mixer channel %1.").arg(chIdx);
		return QJsonObject();
	}

	const auto& effects = ch->m_fxChain.effects();
	if (effIdx < 0 || effIdx >= static_cast<int>(effects.size()))
	{
		ok = false;
		errorMsg = QStringLiteral("effect_index %1 is out of range for channel %2 (has %3 effects).")
			.arg(effIdx).arg(chIdx).arg(effects.size());
		return QJsonObject();
	}

	Effect* eff = effects[effIdx];
	if (!eff)
	{
		ok = false;
		errorMsg = QStringLiteral("Effect at index %1 is null.").arg(effIdx);
		return QJsonObject();
	}

	bool shouldBypass = !eff->isEnabled();
	if (args.contains(QStringLiteral("bypassed")))
	{
		shouldBypass = args.value(QStringLiteral("bypassed")).toBool();
	}

	eff->enabledModel()->setValue(!shouldBypass);
	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("VST effect bypass state updated.");
	data[QStringLiteral("channel_index")] = chIdx;
	data[QStringLiteral("effect_index")] = effIdx;
	data[QStringLiteral("effect_name")] = eff->displayName();
	data[QStringLiteral("bypassed")] = !eff->isEnabled();
	return data;
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------
void registerVstTools(AgentToolRegistry& reg)
{
	// 1. scan_vst_plugins
	{
		QJsonObject props;
		QJsonObject pCustom;
		pCustom[QStringLiteral("type")] = QStringLiteral("string");
		pCustom[QStringLiteral("description")] = QStringLiteral("Optional custom directory path to scan in addition to standard VST directories.");
		props[QStringLiteral("custom_path")] = pCustom;

		reg.registerTool(
			QStringLiteral("scan_vst_plugins"),
			QStringLiteral("Auto-detects installed VST2 (.dll) and VST3 (.vst3) synthesizer and effect plugins on the system."),
			props,
			QJsonArray(),
			handleScanVstPlugins
		);
	}

	// 2. load_vst_instrument
	{
		QJsonObject props;
		QJsonObject pPath;
		pPath[QStringLiteral("type")] = QStringLiteral("string");
		pPath[QStringLiteral("description")] = QStringLiteral("Path to the .dll or .vst3 synthesizer plugin.");
		props[QStringLiteral("plugin_path")] = pPath;

		QJsonObject pTrack;
		pTrack[QStringLiteral("type")] = QStringLiteral("integer");
		pTrack[QStringLiteral("description")] = QStringLiteral("Optional track index to replace, or omit to create a new track.");
		props[QStringLiteral("track_index")] = pTrack;

		QJsonObject pBB;
		pBB[QStringLiteral("type")] = QStringLiteral("boolean");
		pBB[QStringLiteral("description")] = QStringLiteral("Set true to instantiate directly in the Beat+Bassline editor.");
		props[QStringLiteral("in_bb_editor")] = pBB;

		QJsonObject pName;
		pName[QStringLiteral("type")] = QStringLiteral("string");
		pName[QStringLiteral("description")] = QStringLiteral("Custom display name for the track.");
		props[QStringLiteral("name")] = pName;

		QJsonArray req;
		req.append(QStringLiteral("plugin_path"));

		reg.registerTool(
			QStringLiteral("load_vst_instrument"),
			QStringLiteral("Instantiates a third-party VST synth (Surge XT, Vital, etc.) onto an Instrument Track."),
			props,
			req,
			handleLoadVstInstrument
		);
	}

	// 3. add_vst_effect
	{
		QJsonObject props;
		QJsonObject pCh;
		pCh[QStringLiteral("type")] = QStringLiteral("integer");
		pCh[QStringLiteral("description")] = QStringLiteral("Mixer channel index (0 = Master, 1, 2, ...).");
		props[QStringLiteral("channel_index")] = pCh;

		QJsonObject pPath;
		pPath[QStringLiteral("type")] = QStringLiteral("string");
		pPath[QStringLiteral("description")] = QStringLiteral("Path to the effect .dll or .vst3 file.");
		props[QStringLiteral("plugin_path")] = pPath;

		QJsonObject pWetDry;
		pWetDry[QStringLiteral("type")] = QStringLiteral("number");
		pWetDry[QStringLiteral("description")] = QStringLiteral("Mix wet/dry ratio (0.0 to 1.0, default: 1.0).");
		props[QStringLiteral("wet_dry")] = pWetDry;

		QJsonObject pEn;
		pEn[QStringLiteral("type")] = QStringLiteral("boolean");
		pEn[QStringLiteral("description")] = QStringLiteral("Whether effect is enabled initially (default: true).");
		props[QStringLiteral("enabled")] = pEn;

		QJsonArray req;
		req.append(QStringLiteral("channel_index"));
		req.append(QStringLiteral("plugin_path"));

		reg.registerTool(
			QStringLiteral("add_vst_effect"),
			QStringLiteral("Inserts a third-party VST effect (CamelCrusher, Chow Tape, etc.) onto a mixer channel rack."),
			props,
			req,
			handleAddVstEffect
		);
	}

	// 4. get_vst_parameters
	{
		QJsonObject props;
		QJsonObject pTrack;
		pTrack[QStringLiteral("type")] = QStringLiteral("integer");
		pTrack[QStringLiteral("description")] = QStringLiteral("Track index (for VST synths).");
		props[QStringLiteral("track_index")] = pTrack;

		QJsonObject pCh;
		pCh[QStringLiteral("type")] = QStringLiteral("integer");
		pCh[QStringLiteral("description")] = QStringLiteral("Mixer channel index (for VST effects).");
		props[QStringLiteral("channel_index")] = pCh;

		QJsonObject pEff;
		pEff[QStringLiteral("type")] = QStringLiteral("integer");
		pEff[QStringLiteral("description")] = QStringLiteral("Effect index on the mixer channel (for VST effects).");
		props[QStringLiteral("effect_index")] = pEff;

		QJsonObject pQ;
		pQ[QStringLiteral("type")] = QStringLiteral("string");
		pQ[QStringLiteral("description")] = QStringLiteral("Optional query filter by knob name (e.g. 'cutoff', 'mech', 'drive').");
		props[QStringLiteral("query")] = pQ;

		reg.registerTool(
			QStringLiteral("get_vst_parameters"),
			QStringLiteral("Inspects all knobs, parameter indices, names, normalized values, and display texts of a VST plugin."),
			props,
			QJsonArray(),
			handleGetVstParameters
		);
	}

	// 5. set_vst_parameter
	{
		QJsonObject props;
		QJsonObject pTrack;
		pTrack[QStringLiteral("type")] = QStringLiteral("integer");
		pTrack[QStringLiteral("description")] = QStringLiteral("Track index (for VST synths).");
		props[QStringLiteral("track_index")] = pTrack;

		QJsonObject pCh;
		pCh[QStringLiteral("type")] = QStringLiteral("integer");
		pCh[QStringLiteral("description")] = QStringLiteral("Mixer channel index (for VST effects).");
		props[QStringLiteral("channel_index")] = pCh;

		QJsonObject pEff;
		pEff[QStringLiteral("type")] = QStringLiteral("integer");
		pEff[QStringLiteral("description")] = QStringLiteral("Effect index on mixer channel (for VST effects).");
		props[QStringLiteral("effect_index")] = pEff;

		QJsonObject pIdx;
		pIdx[QStringLiteral("type")] = QStringLiteral("integer");
		pIdx[QStringLiteral("description")] = QStringLiteral("Parameter index (from get_vst_parameters).");
		props[QStringLiteral("param_index")] = pIdx;

		QJsonObject pVal;
		pVal[QStringLiteral("type")] = QStringLiteral("number");
		pVal[QStringLiteral("description")] = QStringLiteral("Normalized value between 0.0 and 1.0.");
		props[QStringLiteral("value")] = pVal;

		QJsonArray req;
		req.append(QStringLiteral("param_index"));
		req.append(QStringLiteral("value"));

		reg.registerTool(
			QStringLiteral("set_vst_parameter"),
			QStringLiteral("Tweaks a VST synthesizer or effect knob in real time with normalized 0.0–1.0 values."),
			props,
			req,
			handleSetVstParameter
		);
	}

	// 6. select_vst_program
	{
		QJsonObject props;
		QJsonObject pTrack;
		pTrack[QStringLiteral("type")] = QStringLiteral("integer");
		pTrack[QStringLiteral("description")] = QStringLiteral("Track index (for VST synths).");
		props[QStringLiteral("track_index")] = pTrack;

		QJsonObject pCh;
		pCh[QStringLiteral("type")] = QStringLiteral("integer");
		pCh[QStringLiteral("description")] = QStringLiteral("Mixer channel index (for VST effects).");
		props[QStringLiteral("channel_index")] = pCh;

		QJsonObject pEff;
		pEff[QStringLiteral("type")] = QStringLiteral("integer");
		pEff[QStringLiteral("description")] = QStringLiteral("Effect index on mixer channel (for VST effects).");
		props[QStringLiteral("effect_index")] = pEff;

		QJsonObject pProgIdx;
		pProgIdx[QStringLiteral("type")] = QStringLiteral("integer");
		pProgIdx[QStringLiteral("description")] = QStringLiteral("Preset index (0, 1, 2, ...).");
		props[QStringLiteral("program_index")] = pProgIdx;

		reg.registerTool(
			QStringLiteral("select_vst_program"),
			QStringLiteral("Switches between internal factory sound banks and program presets inside a VST plugin."),
			props,
			QJsonArray(),
			handleSelectVstProgram
		);
	}

	// 7. load_vst_preset_file
	{
		QJsonObject props;
		QJsonObject pTrack;
		pTrack[QStringLiteral("type")] = QStringLiteral("integer");
		pTrack[QStringLiteral("description")] = QStringLiteral("Track index (for VST synths).");
		props[QStringLiteral("track_index")] = pTrack;

		QJsonObject pCh;
		pCh[QStringLiteral("type")] = QStringLiteral("integer");
		pCh[QStringLiteral("description")] = QStringLiteral("Mixer channel index (for VST effects).");
		props[QStringLiteral("channel_index")] = pCh;

		QJsonObject pEff;
		pEff[QStringLiteral("type")] = QStringLiteral("integer");
		pEff[QStringLiteral("description")] = QStringLiteral("Effect index on mixer channel (for VST effects).");
		props[QStringLiteral("effect_index")] = pEff;

		QJsonObject pFile;
		pFile[QStringLiteral("type")] = QStringLiteral("string");
		pFile[QStringLiteral("description")] = QStringLiteral("Path to .fxp or .fxb soundbank file.");
		props[QStringLiteral("file_path")] = pFile;

		QJsonArray req;
		req.append(QStringLiteral("file_path"));

		reg.registerTool(
			QStringLiteral("load_vst_preset_file"),
			QStringLiteral("Loads an external .fxp or .fxb preset/soundbank file into a VST plugin."),
			props,
			req,
			handleLoadVstPresetFile
		);
	}

	// 8. link_vst_to_automation
	{
		QJsonObject props;
		QJsonObject pTrack;
		pTrack[QStringLiteral("type")] = QStringLiteral("integer");
		pTrack[QStringLiteral("description")] = QStringLiteral("Track index (for VST synths).");
		props[QStringLiteral("track_index")] = pTrack;

		QJsonObject pCh;
		pCh[QStringLiteral("type")] = QStringLiteral("integer");
		pCh[QStringLiteral("description")] = QStringLiteral("Mixer channel index (for VST effects).");
		props[QStringLiteral("channel_index")] = pCh;

		QJsonObject pEff;
		pEff[QStringLiteral("type")] = QStringLiteral("integer");
		pEff[QStringLiteral("description")] = QStringLiteral("Effect index on mixer channel (for VST effects).");
		props[QStringLiteral("effect_index")] = pEff;

		QJsonObject pParamIdx;
		pParamIdx[QStringLiteral("type")] = QStringLiteral("integer");
		pParamIdx[QStringLiteral("description")] = QStringLiteral("Parameter index on the VST to automate.");
		props[QStringLiteral("param_index")] = pParamIdx;

		QJsonObject pAutoTrack;
		pAutoTrack[QStringLiteral("type")] = QStringLiteral("integer");
		pAutoTrack[QStringLiteral("description")] = QStringLiteral("Index of the AutomationTrack.");
		props[QStringLiteral("automation_track_index")] = pAutoTrack;

		QJsonObject pClip;
		pClip[QStringLiteral("type")] = QStringLiteral("integer");
		pClip[QStringLiteral("description")] = QStringLiteral("Clip index on the AutomationTrack (default: 0).");
		props[QStringLiteral("clip_index")] = pClip;

		QJsonArray req;
		req.append(QStringLiteral("param_index"));
		req.append(QStringLiteral("automation_track_index"));

		reg.registerTool(
			QStringLiteral("link_vst_to_automation"),
			QStringLiteral("Connects any VST knob directly to an LMMS automation clip for sweeps, LFO wobbles, and dynamic builds."),
			props,
			req,
			handleLinkVstToAutomation
		);
	}

	// 9. send_vst_midi_cc
	{
		QJsonObject props;
		QJsonObject pTrack;
		pTrack[QStringLiteral("type")] = QStringLiteral("integer");
		pTrack[QStringLiteral("description")] = QStringLiteral("Instrument track index hosting the VST synth.");
		props[QStringLiteral("track_index")] = pTrack;

		QJsonObject pCC;
		pCC[QStringLiteral("type")] = QStringLiteral("integer");
		pCC[QStringLiteral("description")] = QStringLiteral("MIDI CC number (e.g. 1 = Mod Wheel, 11 = Expression, 64 = Sustain, 74 = Brightness; or -1 for pitch bend).");
		props[QStringLiteral("cc_number")] = pCC;

		QJsonObject pVal;
		pVal[QStringLiteral("type")] = QStringLiteral("integer");
		pVal[QStringLiteral("description")] = QStringLiteral("Value (0-127 for CC; or -8192 to 8191 for pitch bend).");
		props[QStringLiteral("value")] = pVal;

		QJsonObject pOffset;
		pOffset[QStringLiteral("type")] = QStringLiteral("integer");
		pOffset[QStringLiteral("description")] = QStringLiteral("Tick offset within audio frame buffer (default: 0).");
		props[QStringLiteral("tick_offset")] = pOffset;

		QJsonObject pPitchBend;
		pPitchBend[QStringLiteral("type")] = QStringLiteral("boolean");
		pPitchBend[QStringLiteral("description")] = QStringLiteral("Set true if sending Pitch Bend instead of CC.");
		props[QStringLiteral("is_pitch_bend")] = pPitchBend;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));
		req.append(QStringLiteral("cc_number"));
		req.append(QStringLiteral("value"));

		reg.registerTool(
			QStringLiteral("send_vst_midi_cc"),
			QStringLiteral("Sends real-time MIDI CC controllers (Mod Wheel, Expression, Sustain) and Pitch Bend to a VST synth."),
			props,
			req,
			handleSendVstMidiCC
		);
	}

	// 10. save_vst_preset
	{
		QJsonObject props;
		QJsonObject pTrack;
		pTrack[QStringLiteral("type")] = QStringLiteral("integer");
		pTrack[QStringLiteral("description")] = QStringLiteral("Track index (for VST synths).");
		props[QStringLiteral("track_index")] = pTrack;

		QJsonObject pCh;
		pCh[QStringLiteral("type")] = QStringLiteral("integer");
		pCh[QStringLiteral("description")] = QStringLiteral("Mixer channel index (for VST effects).");
		props[QStringLiteral("channel_index")] = pCh;

		QJsonObject pEff;
		pEff[QStringLiteral("type")] = QStringLiteral("integer");
		pEff[QStringLiteral("description")] = QStringLiteral("Effect index on mixer channel (for VST effects).");
		props[QStringLiteral("effect_index")] = pEff;

		QJsonObject pOut;
		pOut[QStringLiteral("type")] = QStringLiteral("string");
		pOut[QStringLiteral("description")] = QStringLiteral("Destination path for .fxp or .fxb preset file.");
		props[QStringLiteral("output_path")] = pOut;

		QJsonArray req;
		req.append(QStringLiteral("output_path"));

		reg.registerTool(
			QStringLiteral("save_vst_preset"),
			QStringLiteral("Exports the current customized VST sound design patch to an .fxp or .fxb preset file on disk."),
			props,
			req,
			handleSaveVstPreset
		);
	}

	// 11. toggle_vst_gui
	{
		QJsonObject props;
		QJsonObject pTrack;
		pTrack[QStringLiteral("type")] = QStringLiteral("integer");
		pTrack[QStringLiteral("description")] = QStringLiteral("Track index (for VST synths).");
		props[QStringLiteral("track_index")] = pTrack;

		QJsonObject pCh;
		pCh[QStringLiteral("type")] = QStringLiteral("integer");
		pCh[QStringLiteral("description")] = QStringLiteral("Mixer channel index (for VST effects).");
		props[QStringLiteral("channel_index")] = pCh;

		QJsonObject pEff;
		pEff[QStringLiteral("type")] = QStringLiteral("integer");
		pEff[QStringLiteral("description")] = QStringLiteral("Effect index on mixer channel (for VST effects).");
		props[QStringLiteral("effect_index")] = pEff;

		QJsonObject pVis;
		pVis[QStringLiteral("type")] = QStringLiteral("boolean");
		pVis[QStringLiteral("description")] = QStringLiteral("Explicitly set true to show, false to hide, or omit to toggle.");
		props[QStringLiteral("visible")] = pVis;

		reg.registerTool(
			QStringLiteral("toggle_vst_gui"),
			QStringLiteral("Toggles or sets the visibility of a VST plugin GUI editor window on the screen."),
			props,
			QJsonArray(),
			handleToggleVstGui
		);
	}

	// 12. bypass_vst
	{
		QJsonObject props;
		QJsonObject pCh;
		pCh[QStringLiteral("type")] = QStringLiteral("integer");
		pCh[QStringLiteral("description")] = QStringLiteral("Mixer channel index.");
		props[QStringLiteral("channel_index")] = pCh;

		QJsonObject pEff;
		pEff[QStringLiteral("type")] = QStringLiteral("integer");
		pEff[QStringLiteral("description")] = QStringLiteral("Effect index on the mixer channel.");
		props[QStringLiteral("effect_index")] = pEff;

		QJsonObject pBypass;
		pBypass[QStringLiteral("type")] = QStringLiteral("boolean");
		pBypass[QStringLiteral("description")] = QStringLiteral("Set true to bypass, false to activate, or omit to toggle.");
		props[QStringLiteral("bypassed")] = pBypass;

		QJsonArray req;
		req.append(QStringLiteral("channel_index"));
		req.append(QStringLiteral("effect_index"));

		reg.registerTool(
			QStringLiteral("bypass_vst"),
			QStringLiteral("Bypasses or enables an audio effect on a mixer channel for instant A/B sound comparison."),
			props,
			req,
			handleBypassVst
		);
	}
}

} // namespace lmms
