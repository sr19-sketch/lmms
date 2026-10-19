/*
 * AgentInstrumentTools.cpp - Instrument management tools for AI Agent in LMMS
 *
 * Copyright (c) 2026 LMMS Developers
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "AgentInstrumentTools.h"
#include "AgentToolRegistry.h"

#include "Engine.h"
#include "Song.h"
#include "Track.h"
#include "InstrumentTrack.h"
#include "Instrument.h"
#include "PluginFactory.h"
#include "Plugin.h"
#include "ConfigManager.h"
#include "PatternStore.h"
#include "DataFile.h"
#include "AutomatableModel.h"

#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QSet>

namespace lmms
{

extern PluginFactory* getPluginFactory();

static QJsonObject handleListAvailableInstruments(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Q_UNUSED(args);

	PluginFactory* factory = PluginFactory::instance();
	if (!factory)
	{
		factory = getPluginFactory();
	}

	if (!factory)
	{
		ok = false;
		errorMsg = QStringLiteral("PluginFactory is not available.");
		return QJsonObject();
	}

	Plugin::DescriptorList instDescs = factory->descriptors(Plugin::Type::Instrument);

	QJsonArray instruments;
	for (const Plugin::Descriptor* desc : instDescs)
	{
		if (!desc) continue;

		QJsonObject instObj;
		instObj[QStringLiteral("name")] = QString::fromUtf8(desc->name);
		instObj[QStringLiteral("display_name")] = QString::fromUtf8(desc->displayName ? desc->displayName : desc->name);
		instObj[QStringLiteral("description")] = QString::fromUtf8(desc->description ? desc->description : "");
		instObj[QStringLiteral("author")] = QString::fromUtf8(desc->author ? desc->author : "");
		instObj[QStringLiteral("version")] = desc->version;
		instObj[QStringLiteral("supported_file_types")] = QString::fromUtf8(desc->supportedFileTypes ? desc->supportedFileTypes : "");
		instruments.append(instObj);
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("count")] = instruments.size();
	data[QStringLiteral("instruments")] = instruments;
	return data;
}

static QJsonObject handleLoadInstrument(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	QString instrumentName = args.value(QStringLiteral("instrument_name")).toString().trimmed();
	if (instrumentName.isEmpty())
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameter: 'instrument_name'.");
		return QJsonObject();
	}

	PluginFactory* factory = PluginFactory::instance();
	if (!factory) factory = getPluginFactory();

	QString canonicalName = instrumentName;
	if (factory)
	{
		Plugin::DescriptorList instDescs = factory->descriptors(Plugin::Type::Instrument);
		for (const Plugin::Descriptor* desc : instDescs)
		{
			if (desc && (QString::compare(QString::fromUtf8(desc->name), instrumentName, Qt::CaseInsensitive) == 0 ||
			             QString::compare(QString::fromUtf8(desc->displayName), instrumentName, Qt::CaseInsensitive) == 0))
			{
				canonicalName = QString::fromUtf8(desc->name);
				break;
			}
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

	Instrument* inst = it->loadInstrument(canonicalName);
	if (!inst)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to load instrument '%1' (canonical name '%2').").arg(instrumentName, canonicalName);
		return QJsonObject();
	}

	QString presetPath = args.value(QStringLiteral("preset_path")).toString().trimmed();
	bool presetLoaded = false;
	if (!presetPath.isEmpty())
	{
		QFileInfo fi(presetPath);
		if (!fi.exists())
		{
			QString candidate = ConfigManager::inst()->userPresetsDir() + presetPath;
			if (QFile::exists(candidate))
			{
				presetPath = candidate;
				fi.setFile(presetPath);
			}
			else
			{
				ok = false;
				errorMsg = QStringLiteral("Preset file does not exist: %1").arg(presetPath);
				return QJsonObject();
			}
		}
		presetPath = fi.absoluteFilePath();

		if (presetPath.endsWith(QStringLiteral(".xpf"), Qt::CaseInsensitive))
		{
			DataFile df(presetPath);
			it->replaceInstrument(df);
			presetLoaded = true;
		}
		else if (it->instrument())
		{
			it->instrument()->loadFile(presetPath);
			presetLoaded = true;
		}
	}

	song->setModified();
	emit it->dataChanged();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Instrument '%1' loaded successfully on track %2.")
		.arg(it->instrumentName()).arg(targetTrackIdx);
	data[QStringLiteral("track_index")] = targetTrackIdx;
	data[QStringLiteral("instrument_name")] = it->instrumentName();
	data[QStringLiteral("track_name")] = it->name();
	data[QStringLiteral("preset_path")] = presetLoaded ? presetPath : QStringLiteral("");
	return data;
}

static int waveShapeNameToIndex(const QString& name)
{
	QString s = name.toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' ')).remove(QLatin1Char('-'));
	if (s == QStringLiteral("sine")) return 0;
	if (s == QStringLiteral("triangle")) return 1;
	if (s == QStringLiteral("saw") || s == QStringLiteral("sawtooth")) return 2;
	if (s == QStringLiteral("square")) return 3;
	if (s == QStringLiteral("moogsaw")) return 4;
	if (s == QStringLiteral("exponential") || s == QStringLiteral("exp")) return 5;
	if (s == QStringLiteral("noise") || s == QStringLiteral("whitenoise")) return 6;
	return 0;
}

static int filterTypeNameToIndex(const QString& name)
{
	QString s = name.toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' '));
	if (s.contains(QStringLiteral("lowpass")) || s == QStringLiteral("lp")) return 0;
	if (s.contains(QStringLiteral("highpass")) || s == QStringLiteral("hp")) return 1;
	if (s.contains(QStringLiteral("bandpass")) || s == QStringLiteral("bp")) return 2;
	if (s.contains(QStringLiteral("notch"))) return 3;
	return 0;
}

static AutomatableModel* findMatchingModel(const QString& key, const QList<AutomatableModel*>& models)
{
	QString k = key.toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' ')).remove(QLatin1Char('-'));

	// 1. Exact match on simplified name or object name
	for (AutomatableModel* m : models)
	{
		if (!m) continue;
		QString mName = m->displayName().toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' ')).remove(QLatin1Char('-'));
		QString mObj = m->objectName().toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' ')).remove(QLatin1Char('-'));
		if (mName == k || mObj == k)
		{
			return m;
		}
	}

	// 2. Common musical aliases
	QString aliasTarget;
	if (k == QStringLiteral("attack")) aliasTarget = QStringLiteral("envattack");
	else if (k == QStringLiteral("decay")) aliasTarget = QStringLiteral("envdecay");
	else if (k == QStringLiteral("sustain")) aliasTarget = QStringLiteral("envsustain");
	else if (k == QStringLiteral("release")) aliasTarget = QStringLiteral("envrelease");
	else if (k == QStringLiteral("hold")) aliasTarget = QStringLiteral("envhold");
	else if (k == QStringLiteral("predelay")) aliasTarget = QStringLiteral("envpredelay");
	else if (k == QStringLiteral("amount")) aliasTarget = QStringLiteral("envmodamount");
	else if (k == QStringLiteral("cutoff") || k == QStringLiteral("filtercutoff")) aliasTarget = QStringLiteral("cutofffrequency");
	else if (k == QStringLiteral("resonance") || k == QStringLiteral("filterresonance") || k == QStringLiteral("q")) aliasTarget = QStringLiteral("qresonance");
	else if (k == QStringLiteral("filtertype")) aliasTarget = QStringLiteral("filtertype");
	else if (k == QStringLiteral("filterenabled")) aliasTarget = QStringLiteral("filterenabled");
	else if (k == QStringLiteral("osc1wave") || k == QStringLiteral("osc1waveshape")) aliasTarget = QStringLiteral("osc1waveshape");
	else if (k == QStringLiteral("osc2wave") || k == QStringLiteral("osc2waveshape")) aliasTarget = QStringLiteral("osc2waveshape");
	else if (k == QStringLiteral("osc3wave") || k == QStringLiteral("osc3waveshape")) aliasTarget = QStringLiteral("osc3waveshape");
	else if (k == QStringLiteral("osc1vol") || k == QStringLiteral("osc1volume")) aliasTarget = QStringLiteral("osc1volume");
	else if (k == QStringLiteral("osc2vol") || k == QStringLiteral("osc2volume")) aliasTarget = QStringLiteral("osc2volume");
	else if (k == QStringLiteral("osc3vol") || k == QStringLiteral("osc3volume")) aliasTarget = QStringLiteral("osc3volume");

	if (!aliasTarget.isEmpty())
	{
		for (AutomatableModel* m : models)
		{
			if (!m) continue;
			QString mName = m->displayName().toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' ')).remove(QLatin1Char('-'));
			if (mName == aliasTarget || mName.contains(aliasTarget))
			{
				return m;
			}
		}
	}

	// 3. Substring match
	for (AutomatableModel* m : models)
	{
		if (!m) continue;
		QString mName = m->displayName().toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' ')).remove(QLatin1Char('-'));
		QString mObj = m->objectName().toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' ')).remove(QLatin1Char('-'));
		if ((!mName.isEmpty() && mName.contains(k)) || (!mObj.isEmpty() && mObj.contains(k)))
		{
			return m;
		}
	}

	return nullptr;
}

static QJsonObject handleSetInstrumentParams(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	int trackIndex = args.value(QStringLiteral("track_index")).toInt(-1);
	if (trackIndex < 0 || trackIndex >= static_cast<int>(song->tracks().size()))
	{
		ok = false;
		errorMsg = QStringLiteral("Track index %1 is out of range.").arg(trackIndex);
		return QJsonObject();
	}

	Track* t = song->tracks()[trackIndex];
	auto it = dynamic_cast<InstrumentTrack*>(t);
	if (!it)
	{
		ok = false;
		errorMsg = QStringLiteral("Track at index %1 ('%2') is not an InstrumentTrack.").arg(trackIndex).arg(t->name());
		return QJsonObject();
	}

	QJsonObject params = args.value(QStringLiteral("params")).toObject();
	if (params.isEmpty())
	{
		ok = false;
		errorMsg = QStringLiteral("Missing or empty 'params' object.");
		return QJsonObject();
	}

	QList<AutomatableModel*> allModels = it->findChildren<AutomatableModel*>();
	if (it->instrument())
	{
		allModels.append(it->instrument()->findChildren<AutomatableModel*>());
	}

	QJsonObject appliedParams;
	int appliedCount = 0;

	for (auto iter = params.constBegin(); iter != params.constEnd(); ++iter)
	{
		QString key = iter.key();
		QJsonValue val = iter.value();

		AutomatableModel* model = findMatchingModel(key, allModels);
		if (!model) continue;

		float numericVal = 0.0f;
		if (val.isDouble())
		{
			numericVal = static_cast<float>(val.toDouble());
		}
		else if (val.isBool())
		{
			numericVal = val.toBool() ? 1.0f : 0.0f;
		}
		else if (val.isString())
		{
			QString str = val.toString().trimmed();
			if (model->displayName().contains(QStringLiteral("wave"), Qt::CaseInsensitive))
			{
				numericVal = static_cast<float>(waveShapeNameToIndex(str));
			}
			else if (model->displayName().contains(QStringLiteral("filter type"), Qt::CaseInsensitive))
			{
				numericVal = static_cast<float>(filterTypeNameToIndex(str));
			}
			else
			{
				numericVal = str.toFloat();
			}
		}

		model->setValue(numericVal);
		appliedParams[model->displayName()] = model->value<double>();
		appliedCount++;
	}

	song->setModified();
	emit it->dataChanged();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Applied %1 parameter(s) to instrument '%2' on track %3.")
		.arg(appliedCount).arg(it->instrumentName()).arg(trackIndex);
	data[QStringLiteral("track_index")] = trackIndex;
	data[QStringLiteral("instrument_name")] = it->instrumentName();
	data[QStringLiteral("applied_count")] = appliedCount;
	data[QStringLiteral("applied_parameters")] = appliedParams;
	return data;
}

static QJsonObject handleSearchSamples(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Q_UNUSED(errorMsg);

	QString query = args.value(QStringLiteral("query")).toString().trimmed().toLower();
	QString category = args.value(QStringLiteral("category")).toString().trimmed().toLower();
	int maxResults = args.value(QStringLiteral("max_results")).toInt(50);
	if (maxResults <= 0) maxResults = 50;

	QStringList searchDirs;
	searchDirs << ConfigManager::inst()->factorySamplesDir();
	searchDirs << ConfigManager::inst()->userSamplesDir();

	QStringList filters;
	filters << QStringLiteral("*.wav") << QStringLiteral("*.ogg") << QStringLiteral("*.flac")
	        << QStringLiteral("*.mp3") << QStringLiteral("*.ds");

	QJsonArray samples;
	QSet<QString> seenPaths;

	for (const QString& dirPath : searchDirs)
	{
		if (dirPath.isEmpty() || !QDir(dirPath).exists()) continue;

		QDirIterator it(dirPath, filters, QDir::Files, QDirIterator::Subdirectories);
		while (it.hasNext())
		{
			QString filePath = it.next();
			if (seenPaths.contains(filePath)) continue;

			QFileInfo fi(filePath);
			QString fileName = fi.fileName().toLower();
			QString dirName = fi.dir().dirName().toLower();
			QString relPath = QDir(dirPath).relativeFilePath(filePath).toLower();

			if (!category.isEmpty() && !relPath.contains(category) && !dirName.contains(category))
			{
				continue;
			}

			if (!query.isEmpty() && !fileName.contains(query) && !relPath.contains(query))
			{
				continue;
			}

			seenPaths.insert(filePath);

			QJsonObject sampleObj;
			sampleObj[QStringLiteral("name")] = fi.fileName();
			sampleObj[QStringLiteral("path")] = filePath;
			sampleObj[QStringLiteral("category")] = fi.dir().dirName();
			sampleObj[QStringLiteral("size_bytes")] = static_cast<qint64>(fi.size());
			sampleObj[QStringLiteral("extension")] = fi.suffix().toLower();
			samples.append(sampleObj);

			if (samples.size() >= maxResults)
			{
				break;
			}
		}

		if (samples.size() >= maxResults)
		{
			break;
		}
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("count")] = samples.size();
	data[QStringLiteral("query")] = query;
	data[QStringLiteral("category")] = category;
	data[QStringLiteral("samples")] = samples;
	return data;
}

static QJsonObject handleListPresets(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Q_UNUSED(errorMsg);

	QString instrument = args.value(QStringLiteral("instrument_name")).toString().trimmed().toLower();
	QString query = args.value(QStringLiteral("query")).toString().trimmed().toLower();
	int maxResults = args.value(QStringLiteral("max_results")).toInt(50);
	if (maxResults <= 0) maxResults = 50;

	QStringList searchDirs;
	searchDirs << ConfigManager::inst()->factoryPresetsDir();
	searchDirs << ConfigManager::inst()->userPresetsDir();

	QStringList filters;
	filters << QStringLiteral("*.xpf") << QStringLiteral("*.xpt");

	QJsonArray presets;
	QSet<QString> seenPaths;

	for (const QString& dirPath : searchDirs)
	{
		if (dirPath.isEmpty() || !QDir(dirPath).exists()) continue;

		QDirIterator it(dirPath, filters, QDir::Files, QDirIterator::Subdirectories);
		while (it.hasNext())
		{
			QString filePath = it.next();
			if (seenPaths.contains(filePath)) continue;

			QFileInfo fi(filePath);
			QString fileName = fi.baseName().toLower();
			QString parentFolder = fi.dir().dirName().toLower();
			QString relPath = QDir(dirPath).relativeFilePath(filePath).toLower();

			if (!instrument.isEmpty() && !parentFolder.contains(instrument) && !relPath.contains(instrument))
			{
				continue;
			}

			if (!query.isEmpty() && !fileName.contains(query) && !relPath.contains(query))
			{
				continue;
			}

			seenPaths.insert(filePath);

			QJsonObject presetObj;
			presetObj[QStringLiteral("name")] = fi.baseName();
			presetObj[QStringLiteral("path")] = filePath;
			presetObj[QStringLiteral("instrument")] = fi.dir().dirName();
			presetObj[QStringLiteral("size_bytes")] = static_cast<qint64>(fi.size());
			presets.append(presetObj);

			if (presets.size() >= maxResults)
			{
				break;
			}
		}

		if (presets.size() >= maxResults)
		{
			break;
		}
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("count")] = presets.size();
	data[QStringLiteral("instrument_name")] = instrument;
	data[QStringLiteral("query")] = query;
	data[QStringLiteral("presets")] = presets;
	return data;
}

static QJsonObject handleGetInstrumentParams(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	int trackIndex = args.value(QStringLiteral("track_index")).toInt(-1);
	if (trackIndex < 0 || trackIndex >= static_cast<int>(song->tracks().size()))
	{
		ok = false;
		errorMsg = QStringLiteral("Track index %1 is out of range.").arg(trackIndex);
		return QJsonObject();
	}

	Track* t = song->tracks()[trackIndex];
	auto it = dynamic_cast<InstrumentTrack*>(t);
	if (!it)
	{
		ok = false;
		errorMsg = QStringLiteral("Track at index %1 ('%2') is not an InstrumentTrack.").arg(trackIndex).arg(t->name());
		return QJsonObject();
	}

	QList<AutomatableModel*> allModels = it->findChildren<AutomatableModel*>();
	if (it->instrument())
	{
		allModels.append(it->instrument()->findChildren<AutomatableModel*>());
	}

	QJsonObject paramsObj;
	QSet<AutomatableModel*> seen;

	for (AutomatableModel* m : allModels)
	{
		if (!m || seen.contains(m)) continue;
		seen.insert(m);

		QString name = m->displayName();
		if (name.isEmpty()) name = m->objectName();
		if (name.isEmpty()) continue;

		QJsonObject p;
		p[QStringLiteral("value")] = m->value<float>();
		p[QStringLiteral("min")] = m->minValue<float>();
		p[QStringLiteral("max")] = m->maxValue<float>();
		p[QStringLiteral("default")] = m->initValue<float>();
		p[QStringLiteral("display_name")] = m->displayName();
		p[QStringLiteral("object_name")] = m->objectName();

		paramsObj[name] = p;
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("track_index")] = trackIndex;
	data[QStringLiteral("track_name")] = it->name();
	data[QStringLiteral("instrument_name")] = it->instrumentName();
	data[QStringLiteral("param_count")] = paramsObj.size();
	data[QStringLiteral("params")] = paramsObj;
	return data;
}

static QJsonObject handleSetTrackPitchRangeAndGlide(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	int trackIndex = args.value(QStringLiteral("track_index")).toInt(-1);
	if (trackIndex < 0 || trackIndex >= static_cast<int>(song->tracks().size()))
	{
		ok = false;
		errorMsg = QStringLiteral("Track index %1 is out of range.").arg(trackIndex);
		return QJsonObject();
	}

	Track* t = song->tracks()[trackIndex];
	auto it = dynamic_cast<InstrumentTrack*>(t);
	if (!it)
	{
		ok = false;
		errorMsg = QStringLiteral("Track at index %1 ('%2') is not an InstrumentTrack.").arg(trackIndex).arg(t->name());
		return QJsonObject();
	}

	if (args.contains(QStringLiteral("pitch_range")))
	{
		int pitchRange = args.value(QStringLiteral("pitch_range")).toInt(1);
		pitchRange = std::clamp(pitchRange, 1, 60);
		it->pitchRangeModel()->setValue(pitchRange);
	}

	bool glideConfigured = false;
	if (args.contains(QStringLiteral("glide_enabled")) || args.contains(QStringLiteral("glide_time_ms")))
	{
		bool glideEnabled = args.value(QStringLiteral("glide_enabled")).toBool(true);
		double glideTime = args.value(QStringLiteral("glide_time_ms")).toDouble(50.0);

		if (it->instrument())
		{
			QList<AutomatableModel*> models = it->instrument()->findChildren<AutomatableModel*>();
			for (AutomatableModel* m : models)
			{
				if (!m) continue;
				QString n = m->displayName().toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' '));
				QString objN = m->objectName().toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' '));

				if (n.contains(QStringLiteral("porta")) || n.contains(QStringLiteral("glide")) ||
				    objN.contains(QStringLiteral("porta")) || objN.contains(QStringLiteral("glide")))
				{
					if (auto bm = dynamic_cast<BoolModel*>(m))
					{
						bm->setValue(glideEnabled);
						glideConfigured = true;
					}
					else if (n.contains(QStringLiteral("time")) || objN.contains(QStringLiteral("time")))
					{
						m->setValue(static_cast<float>(glideTime));
						glideConfigured = true;
					}
					else if (auto fm = dynamic_cast<FloatModel*>(m))
					{
						fm->setValue(glideEnabled ? static_cast<float>(glideTime) : 0.0f);
						glideConfigured = true;
					}
				}
			}
		}
	}

	song->setModified();
	emit it->dataChanged();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Updated pitch bend range and glide settings for track '%1'.").arg(it->name());
	data[QStringLiteral("track_index")] = trackIndex;
	data[QStringLiteral("pitch_range_semitones")] = it->midiPitchRange();
	data[QStringLiteral("glide_configured")] = glideConfigured;
	return data;
}

static QJsonObject handleLoadSoundfontPatch(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	int trackIndex = args.value(QStringLiteral("track_index")).toInt(-1);
	int bank = args.value(QStringLiteral("bank")).toInt(0);
	int patch = args.value(QStringLiteral("patch")).toInt(0);

	if (trackIndex < 0 || trackIndex >= static_cast<int>(song->tracks().size()))
	{
		ok = false;
		errorMsg = QStringLiteral("Track index %1 is out of range.").arg(trackIndex);
		return QJsonObject();
	}

	Track* t = song->tracks()[trackIndex];
	auto it = dynamic_cast<InstrumentTrack*>(t);
	if (!it)
	{
		ok = false;
		errorMsg = QStringLiteral("Track at index %1 ('%2') is not an InstrumentTrack.").arg(trackIndex).arg(t->name());
		return QJsonObject();
	}

	Instrument* inst = it->instrument();
	if (!inst)
	{
		ok = false;
		errorMsg = QStringLiteral("Track '%1' does not have an active instrument loaded.").arg(it->name());
		return QJsonObject();
	}

	bool applied = false;
	AutomatableModel* bankM = inst->childModel("bank");
	AutomatableModel* patchM = inst->childModel("patch");

	if (bankM)
	{
		bankM->setValue(bank);
		applied = true;
	}
	if (patchM)
	{
		patchM->setValue(patch);
		applied = true;
	}

	if (!applied)
	{
		QList<AutomatableModel*> models = inst->findChildren<AutomatableModel*>();
		for (AutomatableModel* m : models)
		{
			if (!m) continue;
			QString n = m->objectName().toLower();
			if (n == QStringLiteral("bank") || n.contains(QStringLiteral("bank")))
			{
				m->setValue(bank);
				applied = true;
			}
			else if (n == QStringLiteral("patch") || n.contains(QStringLiteral("patch")) || n == QStringLiteral("program"))
			{
				m->setValue(patch);
				applied = true;
			}
		}
	}

	QMetaObject::invokeMethod(inst, "updatePatch");

	song->setModified();
	emit it->dataChanged();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Loaded SoundFont patch (Bank %1, Patch %2) on track '%3'.")
		.arg(bank).arg(patch).arg(it->name());
	data[QStringLiteral("track_index")] = trackIndex;
	data[QStringLiteral("instrument_name")] = it->instrumentName();
	data[QStringLiteral("bank")] = bank;
	data[QStringLiteral("patch")] = patch;
	data[QStringLiteral("success")] = applied;
	return data;
}

void registerInstrumentTools(AgentToolRegistry& reg)
{
	// 1. list_available_instruments
	{
		reg.registerTool(
			QStringLiteral("list_available_instruments"),
			QStringLiteral("Discovers all instrument plugins available in LMMS (e.g. TripleOscillator, BitInvader, sf2player, Kicker, NesCaline, SID, Monstr, Vibed, etc.)."),
			QJsonObject(),
			QJsonArray(),
			handleListAvailableInstruments
		);
	}

	// 2. load_instrument
	{
		QJsonObject props;
		QJsonObject propInst;
		propInst[QStringLiteral("type")] = QStringLiteral("string");
		propInst[QStringLiteral("description")] = QStringLiteral("Name of the instrument plugin to load (e.g. 'TripleOscillator', 'sf2player', 'BitInvader', 'kicker').");
		props[QStringLiteral("instrument_name")] = propInst;

		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Optional index of an existing track to replace the instrument on. If omitted, creates a new InstrumentTrack.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propPreset;
		propPreset[QStringLiteral("type")] = QStringLiteral("string");
		propPreset[QStringLiteral("description")] = QStringLiteral("Optional path to an instrument preset (.xpf) or soundfont (.sf2) file to load.");
		QJsonObject propInBB;
		propInBB[QStringLiteral("type")] = QStringLiteral("boolean");
		propInBB[QStringLiteral("description")] = QStringLiteral("If true, creates or loads the instrument inside the Beat+Bassline (Pattern) Editor rather than Song Editor.");
		props[QStringLiteral("in_bb_editor")] = propInBB;

		QJsonArray required;
		required.append(QStringLiteral("instrument_name"));

		reg.registerTool(
			QStringLiteral("load_instrument"),
			QStringLiteral("Instantiates an instrument plugin directly onto an InstrumentTrack in LMMS."),
			props,
			required,
			handleLoadInstrument
		);
	}

	// 3. set_instrument_params
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the InstrumentTrack to modify.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propParams;
		propParams[QStringLiteral("type")] = QStringLiteral("object");
		propParams[QStringLiteral("description")] = QStringLiteral("Key-value map of parameter adjustments. Supports envelope attack/decay/sustain/release/hold, filter cutoffs, resonance, filter type, oscillator waveforms (e.g. 'saw', 'sine', 'square'), volumes, coarse detuning, and plugin-specific parameters.");
		props[QStringLiteral("params")] = propParams;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));
		req.append(QStringLiteral("params"));

		reg.registerTool(
			QStringLiteral("set_instrument_params"),
			QStringLiteral("Tweaks synthesizer parameters, envelopes, filter cutoffs, and oscillator wave shapes directly on any instrument track."),
			props,
			req,
			handleSetInstrumentParams
		);
	}

	// 4. search_samples
	{
		QJsonObject props;
		QJsonObject propQuery;
		propQuery[QStringLiteral("type")] = QStringLiteral("string");
		propQuery[QStringLiteral("description")] = QStringLiteral("Optional search term (e.g. 'kick', 'snare', 'hihat', 'bass', '808').");
		props[QStringLiteral("query")] = propQuery;

		QJsonObject propCat;
		propCat[QStringLiteral("type")] = QStringLiteral("string");
		propCat[QStringLiteral("description")] = QStringLiteral("Optional sample category folder (e.g. 'drums', 'basses', 'effects', 'beats').");
		props[QStringLiteral("category")] = propCat;

		QJsonObject propMax;
		propMax[QStringLiteral("type")] = QStringLiteral("integer");
		propMax[QStringLiteral("description")] = QStringLiteral("Maximum results to return (default: 50).");
		props[QStringLiteral("max_results")] = propMax;

		reg.registerTool(
			QStringLiteral("search_samples"),
			QStringLiteral("Searches LMMS's built-in factory library and user directories for drum samples and audio files."),
			props,
			QJsonArray(),
			handleSearchSamples
		);
	}

	// 5. list_presets
	{
		QJsonObject props;
		QJsonObject propInst;
		propInst[QStringLiteral("type")] = QStringLiteral("string");
		propInst[QStringLiteral("description")] = QStringLiteral("Optional instrument plugin name to filter presets for (e.g. 'TripleOscillator', 'Kicker', 'ZynAddSubFX').");
		props[QStringLiteral("instrument_name")] = propInst;

		QJsonObject propQuery;
		propQuery[QStringLiteral("type")] = QStringLiteral("string");
		propQuery[QStringLiteral("description")] = QStringLiteral("Optional query to filter preset names.");
		props[QStringLiteral("query")] = propQuery;

		QJsonObject propMax;
		propMax[QStringLiteral("type")] = QStringLiteral("integer");
		propMax[QStringLiteral("description")] = QStringLiteral("Maximum results to return (default: 50).");
		props[QStringLiteral("max_results")] = propMax;

		reg.registerTool(
			QStringLiteral("list_presets"),
			QStringLiteral("Lists factory and user presets (.xpf) for sound design discovery across LMMS instruments."),
			props,
			QJsonArray(),
			handleListPresets
		);
	}

	// 6. get_instrument_params
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the target InstrumentTrack.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));

		reg.registerTool(
			QStringLiteral("get_instrument_params"),
			QStringLiteral("Queries the current parameter values, envelope settings (AHDSR), filter cutoffs, and oscillator settings of an instrument track."),
			props,
			req,
			handleGetInstrumentParams
		);
	}

	// 7. set_track_pitch_range_and_glide
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the target InstrumentTrack.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propRange;
		propRange[QStringLiteral("type")] = QStringLiteral("integer");
		propRange[QStringLiteral("description")] = QStringLiteral("Pitch bend range in semitones (1 to 60).");
		props[QStringLiteral("pitch_range")] = propRange;

		QJsonObject propGlide;
		propGlide[QStringLiteral("type")] = QStringLiteral("boolean");
		propGlide[QStringLiteral("description")] = QStringLiteral("Enable or disable glide/portamento.");
		props[QStringLiteral("glide_enabled")] = propGlide;

		QJsonObject propTime;
		propTime[QStringLiteral("type")] = QStringLiteral("number");
		propTime[QStringLiteral("description")] = QStringLiteral("Glide duration/time in milliseconds.");
		props[QStringLiteral("glide_time_ms")] = propTime;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));

		reg.registerTool(
			QStringLiteral("set_track_pitch_range_and_glide"),
			QStringLiteral("Sets the pitch bend range (e.g. 12 or 24 semitones) and configures polyphonic/monophonic glide (portamento) for 808s and synths."),
			props,
			req,
			handleSetTrackPitchRangeAndGlide
		);
	}

	// 8. load_soundfont_patch
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the target InstrumentTrack running Sf2Player.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propBank;
		propBank[QStringLiteral("type")] = QStringLiteral("integer");
		propBank[QStringLiteral("description")] = QStringLiteral("SoundFont bank number (0 to 128).");
		props[QStringLiteral("bank")] = propBank;

		QJsonObject propPatch;
		propPatch[QStringLiteral("type")] = QStringLiteral("integer");
		propPatch[QStringLiteral("description")] = QStringLiteral("SoundFont patch/program number (0 to 127, e.g. 0 = Acoustic Grand Piano, 25 = Steel Guitar).");
		props[QStringLiteral("patch")] = propPatch;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));
		req.append(QStringLiteral("patch"));

		reg.registerTool(
			QStringLiteral("load_soundfont_patch"),
			QStringLiteral("Selects a specific SoundFont Bank and Patch number inside an .sf2 / .sf3 SoundFont player instrument."),
			props,
			req,
			handleLoadSoundfontPatch
		);
	}
}

} // namespace lmms
