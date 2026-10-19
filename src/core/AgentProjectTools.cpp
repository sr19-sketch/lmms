/*
 * AgentProjectTools.cpp - Project management tools for AI Agent in LMMS
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

#include "AgentProjectTools.h"
#include "AgentToolRegistry.h"

#include "Engine.h"
#include "Song.h"
#include "Track.h"
#include "InstrumentTrack.h"
#include "SampleTrack.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "GuiApplication.h"
#include "MainWindow.h"
#include "ProjectRenderer.h"
#include "OutputSettings.h"
#include "AudioEngine.h"
#include "Timeline.h"
#include "MeterModel.h"
#include "TimePos.h"
#include "ImportFilter.h"
#include "Plugin.h"

#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <filesystem>

namespace lmms
{

static QJsonObject handleCreateNewProject(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	bool force = args.value(QStringLiteral("force")).toBool(false);
	QString templatePath = args.value(QStringLiteral("template_path")).toString().trimmed();

	if (!force && song->isModified())
	{
		ok = false;
		errorMsg = QStringLiteral("Current project has unsaved changes. Pass force=true to discard changes, or call save_project first.");
		return QJsonObject();
	}

	if (song->isPlaying())
	{
		song->stop();
	}

	if (!templatePath.isEmpty())
	{
		QFileInfo fi(templatePath);
		if (!fi.exists())
		{
			ok = false;
			errorMsg = QStringLiteral("Specified template file does not exist: %1").arg(templatePath);
			return QJsonObject();
		}
		song->createNewProjectFromTemplate(templatePath);
	}
	else
	{
		song->createNewProject();
	}

	if (gui::getGUI() && gui::getGUI()->mainWindow())
	{
		if (gui::getGUI()->mainWindow()->getSession() == gui::MainWindow::SessionState::Recover)
		{
			gui::getGUI()->mainWindow()->sessionCleanup();
		}
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("New project created successfully.");
	data[QStringLiteral("file_path")] = song->projectFileName();
	data[QStringLiteral("bpm")] = song->getTempo();
	data[QStringLiteral("track_count")] = static_cast<int>(song->tracks().size());
	data[QStringLiteral("is_modified")] = song->isModified();
	return data;
}

static QJsonObject handleSaveProject(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	QString filePath = args.value(QStringLiteral("file_path")).toString().trimmed();
	bool bundle = args.value(QStringLiteral("bundle")).toBool(false);

	if (filePath.isEmpty())
	{
		filePath = song->projectFileName();
	}

	if (filePath.isEmpty())
	{
		QString projDir = ConfigManager::inst()->userProjectsDir();
		QDir().mkpath(projDir);
		filePath = projDir + QLatin1String("/agent_project.mmpz");
	}

	QFileInfo fi(filePath);
	if (!fi.isAbsolute())
	{
		filePath = QDir(ConfigManager::inst()->userProjectsDir()).absoluteFilePath(filePath);
		fi.setFile(filePath);
	}

	QDir parentDir = fi.dir();
	if (!parentDir.exists())
	{
		parentDir.mkpath(QStringLiteral("."));
	}

	DataFile dataFile(DataFile::Type::SongProject);
	QString fullPathWithExt = dataFile.nameWithExtension(filePath);
	bool success = song->saveProjectFile(fullPathWithExt, bundle);

	if (!success)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to save project to '%1'.").arg(filePath);
		return QJsonObject();
	}

	if (gui::getGUI() && gui::getGUI()->mainWindow())
	{
		if (gui::getGUI()->mainWindow()->getSession() == gui::MainWindow::SessionState::Recover)
		{
			gui::getGUI()->mainWindow()->sessionCleanup();
		}
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Project saved successfully.");
	data[QStringLiteral("file_path")] = fullPathWithExt;
	data[QStringLiteral("is_bundle")] = bundle;
	data[QStringLiteral("is_modified")] = song->isModified();
	return data;
}

static QJsonObject handleGetProjectInfo(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Q_UNUSED(args);

	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	QJsonObject data;
	data[QStringLiteral("file_path")] = song->projectFileName();
	data[QStringLiteral("bpm")] = song->getTempo();
	data[QStringLiteral("master_volume")] = song->masterVolume();
	data[QStringLiteral("master_pitch")] = song->masterPitch();
	data[QStringLiteral("is_modified")] = song->isModified();
	data[QStringLiteral("is_playing")] = song->isPlaying();
	data[QStringLiteral("is_paused")] = song->isPaused();
	data[QStringLiteral("is_stopped")] = song->isStopped();

	const auto& tracks = song->tracks();
	QJsonArray tracksArray;
	for (size_t i = 0; i < tracks.size(); ++i)
	{
		Track* t = tracks[i];
		if (!t) continue;

		QJsonObject trackObj;
		trackObj[QStringLiteral("index")] = static_cast<int>(i);
		trackObj[QStringLiteral("name")] = t->name();
		trackObj[QStringLiteral("type")] = t->displayName();
		trackObj[QStringLiteral("muted")] = t->isMuted();
		trackObj[QStringLiteral("solo")] = t->isSolo();

		if (auto it = dynamic_cast<InstrumentTrack*>(t))
		{
			trackObj[QStringLiteral("instrument_type")] = it->instrumentName();
			trackObj[QStringLiteral("volume")] = it->volumeModel()->value();
			trackObj[QStringLiteral("panning")] = it->panningModel()->value();
			trackObj[QStringLiteral("pitch")] = it->pitchModel()->value();
			trackObj[QStringLiteral("mixer_channel")] = it->mixerChannelModel()->value();
		}
		else if (auto st = dynamic_cast<SampleTrack*>(t))
		{
			trackObj[QStringLiteral("mixer_channel")] = st->mixerChannelModel()->value();
		}

		tracksArray.append(trackObj);
	}

	data[QStringLiteral("track_count")] = static_cast<int>(tracks.size());
	data[QStringLiteral("tracks")] = tracksArray;

	ok = true;
	return data;
}

static QJsonObject handleTransportPlay(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Q_UNUSED(args);
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	song->playSong();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Playback started.");
	data[QStringLiteral("is_playing")] = true;
	data[QStringLiteral("is_paused")] = false;
	data[QStringLiteral("is_stopped")] = false;
	data[QStringLiteral("bar")] = song->getBars();
	data[QStringLiteral("tick")] = song->getTicks();
	data[QStringLiteral("bpm")] = song->getTempo();
	return data;
}

static QJsonObject handleTransportStop(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Q_UNUSED(args);
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	song->stop();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Playback stopped.");
	data[QStringLiteral("is_playing")] = false;
	data[QStringLiteral("is_paused")] = false;
	data[QStringLiteral("is_stopped")] = true;
	data[QStringLiteral("bar")] = song->getBars();
	data[QStringLiteral("tick")] = song->getTicks();
	return data;
}

static QJsonObject handleTransportPause(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Q_UNUSED(args);
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	song->togglePause();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = song->isPaused() ? QStringLiteral("Playback paused.") : QStringLiteral("Playback resumed.");
	data[QStringLiteral("is_playing")] = song->isPlaying();
	data[QStringLiteral("is_paused")] = song->isPaused();
	data[QStringLiteral("is_stopped")] = song->isStopped();
	data[QStringLiteral("bar")] = song->getBars();
	data[QStringLiteral("tick")] = song->getTicks();
	return data;
}

static QJsonObject handleTransportSeek(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	double bar = args.value(QStringLiteral("bar")).toDouble(0.0);
	int tick = args.value(QStringLiteral("tick")).toInt(0);

	int tpb = song->ticksPerBar();
	tick_t targetTick = static_cast<tick_t>(bar * tpb) + tick;
	if (targetTick < 0) targetTick = 0;

	song->setPlayPos(targetTick);

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Playhead position moved successfully.");
	data[QStringLiteral("bar")] = song->getBars();
	data[QStringLiteral("tick")] = song->getTicks();
	data[QStringLiteral("total_ticks")] = static_cast<int>(targetTick);
	return data;
}

static QJsonObject handleTransportSetLoop(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	double startBar = args.value(QStringLiteral("start_bar")).toDouble(0.0);
	double endBar = args.value(QStringLiteral("end_bar")).toDouble(4.0);
	bool enabled = args.value(QStringLiteral("enabled")).toBool(true);

	if (endBar < startBar)
	{
		std::swap(startBar, endBar);
	}

	int tpb = song->ticksPerBar();
	TimePos startPos(static_cast<tick_t>(startBar * tpb));
	TimePos endPos(static_cast<tick_t>(endBar * tpb));

	Timeline& tl = song->getTimeline();
	tl.setLoopPoints(startPos, endPos);
	tl.setLoopEnabled(enabled);

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Loop section updated successfully.");
	data[QStringLiteral("start_bar")] = startBar;
	data[QStringLiteral("end_bar")] = endBar;
	data[QStringLiteral("enabled")] = enabled;
	return data;
}

static QJsonObject handleSetTempo(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	bool changed = false;

	if (args.contains(QStringLiteral("bpm")))
	{
		double bpm = args.value(QStringLiteral("bpm")).toDouble();
		if (bpm >= MinTempo && bpm <= MaxTempo)
		{
			song->tempoModel().setValue(static_cast<int>(bpm));
			changed = true;
		}
		else
		{
			ok = false;
			errorMsg = QStringLiteral("BPM value %1 is out of range [%2, %3].").arg(bpm).arg(MinTempo).arg(MaxTempo);
			return QJsonObject();
		}
	}

	if (args.contains(QStringLiteral("time_signature_num")))
	{
		int num = args.value(QStringLiteral("time_signature_num")).toInt();
		if (num > 0 && num <= 32)
		{
			song->getTimeSigModel().setNumerator(num);
			changed = true;
		}
	}

	if (args.contains(QStringLiteral("time_signature_denom")))
	{
		int denom = args.value(QStringLiteral("time_signature_denom")).toInt();
		if (denom > 0 && denom <= 32)
		{
			song->getTimeSigModel().setDenominator(denom);
			changed = true;
		}
	}

	if (changed)
	{
		song->setModified();
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Tempo and time signature updated.");
	data[QStringLiteral("bpm")] = song->getTempo();
	data[QStringLiteral("time_signature_num")] = song->getTimeSigModel().getNumerator();
	data[QStringLiteral("time_signature_denom")] = song->getTimeSigModel().getDenominator();
	return data;
}

static QJsonObject handleExportAudio(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	if (!Engine::audioEngine())
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS AudioEngine is not available.");
		return QJsonObject();
	}

	QString outputFile = args.value(QStringLiteral("output_file")).toString().trimmed();
	if (outputFile.isEmpty())
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameter: 'output_file'.");
		return QJsonObject();
	}

	QString formatStr = args.value(QStringLiteral("format")).toString(QStringLiteral("wav")).trimmed().toLower();
	ProjectRenderer::ExportFileFormat eff = ProjectRenderer::ExportFileFormat::Wave;
	if (formatStr == QStringLiteral("mp3"))
	{
		eff = ProjectRenderer::ExportFileFormat::MP3;
	}
	else if (formatStr == QStringLiteral("ogg"))
	{
		eff = ProjectRenderer::ExportFileFormat::Ogg;
	}
	else if (formatStr == QStringLiteral("flac"))
	{
		eff = ProjectRenderer::ExportFileFormat::Flac;
	}
	else if (formatStr == QStringLiteral("wav") || formatStr == QStringLiteral("wave"))
	{
		eff = ProjectRenderer::ExportFileFormat::Wave;
	}
	else
	{
		ok = false;
		errorMsg = QStringLiteral("Unsupported audio format '%1'. Supported: 'wav', 'mp3', 'ogg', 'flac'.").arg(formatStr);
		return QJsonObject();
	}

	QFileInfo fi(outputFile);
	if (!fi.isAbsolute())
	{
		QString baseDir = song->projectFileName().isEmpty()
			? ConfigManager::inst()->userProjectsDir()
			: QFileInfo(song->projectFileName()).dir().absolutePath();
		outputFile = QDir(baseDir).absoluteFilePath(outputFile);
		fi.setFile(outputFile);
	}

	QString expectedExt = ProjectRenderer::getFileExtensionFromFormat(eff);
	if (!outputFile.endsWith(expectedExt, Qt::CaseInsensitive))
	{
		outputFile += expectedExt;
		fi.setFile(outputFile);
	}

	QDir parentDir = fi.dir();
	if (!parentDir.exists())
	{
		parentDir.mkpath(QStringLiteral("."));
	}

	int sampleRate = args.value(QStringLiteral("samplerate")).toInt(44100);
	if (sampleRate <= 0) sampleRate = 44100;

	int bitDepthVal = args.value(QStringLiteral("bit_depth")).toInt(16);
	OutputSettings::BitDepth bitDepth = OutputSettings::BitDepth::Depth16Bit;
	if (bitDepthVal == 24)
	{
		bitDepth = OutputSettings::BitDepth::Depth24Bit;
	}
	else if (bitDepthVal == 32)
	{
		bitDepth = OutputSettings::BitDepth::Depth32Bit;
	}

	OutputSettings outputSettings(sampleRate, 192, bitDepth, OutputSettings::StereoMode::Stereo);

	bool wasPlaying = song->isPlaying();
	if (wasPlaying)
	{
		song->stop();
	}

	Engine::audioEngine()->storeAudioDevice();

	{
		ProjectRenderer renderer(outputSettings, eff, outputFile);
		if (!renderer.isReady())
		{
			Engine::audioEngine()->restoreAudioDevice();
			ok = false;
			errorMsg = QStringLiteral("Failed to initialize audio encoder for file '%1' with format '%2'.").arg(outputFile, formatStr);
			return QJsonObject();
		}

		renderer.startProcessing();
		renderer.wait();
	}

	Engine::audioEngine()->restoreAudioDevice();

	QFileInfo outFi(outputFile);
	if (!outFi.exists() || outFi.size() == 0)
	{
		ok = false;
		errorMsg = QStringLiteral("Rendering process did not produce an output file at '%1'.").arg(outputFile);
		return QJsonObject();
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Audio exported successfully.");
	data[QStringLiteral("output_file")] = outputFile;
	data[QStringLiteral("format")] = formatStr;
	data[QStringLiteral("samplerate")] = sampleRate;
	data[QStringLiteral("bit_depth")] = bitDepthVal;
	data[QStringLiteral("file_size_bytes")] = static_cast<qint64>(outFi.size());
	data[QStringLiteral("bars")] = song->length();
	return data;
}

struct TimelineMarkerInfo
{
	double bar;
	QString name;
};
static QList<TimelineMarkerInfo> s_timelineMarkers;

static QJsonObject handleAddTimelineMarker(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	double bar = args.value(QStringLiteral("bar")).toDouble(0.0);
	QString name = args.value(QStringLiteral("name")).toString().trimmed();
	if (name.isEmpty())
	{
		name = QStringLiteral("Marker at Bar %1").arg(bar);
	}

	bool updated = false;
	for (auto& m : s_timelineMarkers)
	{
		if (std::abs(m.bar - bar) < 0.001)
		{
			m.name = name;
			updated = true;
			break;
		}
	}

	if (!updated)
	{
		s_timelineMarkers.append({ bar, name });
	}

	std::sort(s_timelineMarkers.begin(), s_timelineMarkers.end(), [](const TimelineMarkerInfo& a, const TimelineMarkerInfo& b) {
		return a.bar < b.bar;
	});

	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Timeline marker '%1' added at bar %2.").arg(name).arg(bar);
	data[QStringLiteral("bar")] = bar;
	data[QStringLiteral("name")] = name;
	data[QStringLiteral("total_markers")] = s_timelineMarkers.size();
	return data;
}

static QJsonObject handleGetTimelineMarkers(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Q_UNUSED(args);
	Q_UNUSED(errorMsg);

	QJsonArray markersArr;
	for (const auto& m : s_timelineMarkers)
	{
		QJsonObject mo;
		mo[QStringLiteral("bar")] = m.bar;
		mo[QStringLiteral("name")] = m.name;
		markersArr.append(mo);
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("count")] = markersArr.size();
	data[QStringLiteral("markers")] = markersArr;
	return data;
}

static QJsonObject handleImportMidi(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	QString filePath = args.value(QStringLiteral("file_path")).toString().trimmed();
	if (filePath.isEmpty())
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameter: 'file_path'.");
		return QJsonObject();
	}

	QFileInfo fi(filePath);
	if (!fi.exists())
	{
		QString userProjDir = ConfigManager::inst()->userProjectsDir();
		QString testPath = userProjDir + QLatin1Char('/') + filePath;
		if (QFileInfo::exists(testPath))
		{
			filePath = testPath;
			fi = QFileInfo(filePath);
		}
	}

	if (!fi.exists())
	{
		ok = false;
		errorMsg = QStringLiteral("MIDI file does not exist: '%1'.").arg(filePath);
		return QJsonObject();
	}

	int prevTrackCount = static_cast<int>(song->tracks().size());
	ImportFilter::import(filePath, song);
	int newTrackCount = static_cast<int>(song->tracks().size());
	int addedTracks = newTrackCount - prevTrackCount;

	song->updateLength();
	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("MIDI file imported successfully. %1 tracks created.").arg(addedTracks);
	data[QStringLiteral("file_path")] = filePath;
	data[QStringLiteral("tracks_added")] = addedTracks;
	data[QStringLiteral("total_tracks")] = newTrackCount;
	return data;
}

static QJsonObject handleExportMidi(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	QString filePath = args.value(QStringLiteral("file_path")).toString().trimmed();
	if (filePath.isEmpty())
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameter: 'file_path'.");
		return QJsonObject();
	}

	QFileInfo fi(filePath);
	if (fi.isRelative())
	{
		QString userProjDir = ConfigManager::inst()->userProjectsDir();
		filePath = userProjDir + QLatin1Char('/') + filePath;
		fi = QFileInfo(filePath);
	}

	if (!filePath.endsWith(QStringLiteral(".mid"), Qt::CaseInsensitive) &&
	    !filePath.endsWith(QStringLiteral(".midi"), Qt::CaseInsensitive))
	{
		filePath += QStringLiteral(".mid");
		fi = QFileInfo(filePath);
	}

	fi.dir().mkpath(QStringLiteral("."));

	song->exportProjectMidi(std::filesystem::path(filePath.toStdWString()));

	QFileInfo outFi(filePath);
	if (!outFi.exists() || outFi.size() == 0)
	{
		ok = false;
		errorMsg = QStringLiteral("MIDI export failed to produce a file at '%1'.").arg(filePath);
		return QJsonObject();
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("MIDI file exported successfully.");
	data[QStringLiteral("file_path")] = filePath;
	data[QStringLiteral("file_size_bytes")] = static_cast<qint64>(outFi.size());
	return data;
}

void registerProjectTools(AgentToolRegistry& reg)
{
	// 1. create_new_project
	{
		QJsonObject props;
		QJsonObject propForce;
		propForce[QStringLiteral("type")] = QStringLiteral("boolean");
		propForce[QStringLiteral("description")] = QStringLiteral("If true, discards any unsaved changes without error (default: false).");
		props[QStringLiteral("force")] = propForce;

		QJsonObject propTemplate;
		propTemplate[QStringLiteral("type")] = QStringLiteral("string");
		propTemplate[QStringLiteral("description")] = QStringLiteral("Optional path to a project template (.mpt / .mmpz) to initialize from.");
		props[QStringLiteral("template_path")] = propTemplate;

		reg.registerTool(
			QStringLiteral("create_new_project"),
			QStringLiteral("Creates a brand new LMMS project directly in the engine, resetting all tracks and timeline state."),
			props,
			QJsonArray(),
			handleCreateNewProject
		);
	}

	// 2. save_project
	{
		QJsonObject props;
		QJsonObject propPath;
		propPath[QStringLiteral("type")] = QStringLiteral("string");
		propPath[QStringLiteral("description")] = QStringLiteral("File path to save the project to (.mmpz, .mmp, or .mmpb). If omitted, saves to the current file path or default user project.");
		props[QStringLiteral("file_path")] = propPath;

		QJsonObject propBundle;
		propBundle[QStringLiteral("type")] = QStringLiteral("boolean");
		propBundle[QStringLiteral("description")] = QStringLiteral("Save as a self-contained LMMS project bundle (.mmpb) including all sample files (default: false).");
		props[QStringLiteral("bundle")] = propBundle;

		reg.registerTool(
			QStringLiteral("save_project"),
			QStringLiteral("Saves the current LMMS project to disk as a standard project file (.mmpz/.mmp) or project bundle (.mmpb)."),
			props,
			QJsonArray(),
			handleSaveProject
		);
	}

	// 3. get_project_info
	{
		reg.registerTool(
			QStringLiteral("get_project_info"),
			QStringLiteral("Retrieves detailed state and metadata of the currently loaded LMMS project."),
			QJsonObject(),
			QJsonArray(),
			handleGetProjectInfo
		);
	}

	// 4. transport_play
	{
		reg.registerTool(
			QStringLiteral("transport_play"),
			QStringLiteral("Starts project playback from current playhead position in the LMMS engine."),
			QJsonObject(),
			QJsonArray(),
			handleTransportPlay
		);
	}

	// 5. transport_stop
	{
		reg.registerTool(
			QStringLiteral("transport_stop"),
			QStringLiteral("Stops project playback and resets playback handles."),
			QJsonObject(),
			QJsonArray(),
			handleTransportStop
		);
	}

	// 6. transport_pause
	{
		reg.registerTool(
			QStringLiteral("transport_pause"),
			QStringLiteral("Pauses or resumes project playback without resetting playhead position."),
			QJsonObject(),
			QJsonArray(),
			handleTransportPause
		);
	}

	// 7. transport_seek
	{
		QJsonObject props;
		QJsonObject propBar;
		propBar[QStringLiteral("type")] = QStringLiteral("number");
		propBar[QStringLiteral("description")] = QStringLiteral("Target timeline position in bars (0-indexed).");
		props[QStringLiteral("bar")] = propBar;

		QJsonObject propTick;
		propTick[QStringLiteral("type")] = QStringLiteral("integer");
		propTick[QStringLiteral("description")] = QStringLiteral("Optional tick offset within the bar (default: 0).");
		props[QStringLiteral("tick")] = propTick;

		QJsonArray req;
		req.append(QStringLiteral("bar"));

		reg.registerTool(
			QStringLiteral("transport_seek"),
			QStringLiteral("Moves the playhead to audition specific sections in bars and ticks."),
			props,
			req,
			handleTransportSeek
		);
	}

	// 8. transport_set_loop
	{
		QJsonObject props;
		QJsonObject propStart;
		propStart[QStringLiteral("type")] = QStringLiteral("number");
		propStart[QStringLiteral("description")] = QStringLiteral("Start bar of loop region (0-indexed).");
		props[QStringLiteral("start_bar")] = propStart;

		QJsonObject propEnd;
		propEnd[QStringLiteral("type")] = QStringLiteral("number");
		propEnd[QStringLiteral("description")] = QStringLiteral("End bar of loop region.");
		props[QStringLiteral("end_bar")] = propEnd;

		QJsonObject propEn;
		propEn[QStringLiteral("type")] = QStringLiteral("boolean");
		propEn[QStringLiteral("description")] = QStringLiteral("Enable or disable loop playback (default: true).");
		props[QStringLiteral("enabled")] = propEn;

		QJsonArray req;
		req.append(QStringLiteral("start_bar"));
		req.append(QStringLiteral("end_bar"));

		reg.registerTool(
			QStringLiteral("transport_set_loop"),
			QStringLiteral("Sets and enables/disables the loop region for arrangement playback."),
			props,
			req,
			handleTransportSetLoop
		);
	}

	// 9. set_tempo
	{
		QJsonObject props;
		QJsonObject propBpm;
		propBpm[QStringLiteral("type")] = QStringLiteral("number");
		propBpm[QStringLiteral("description")] = QStringLiteral("Project tempo in Beats Per Minute (BPM, 10 to 999).");
		props[QStringLiteral("bpm")] = propBpm;

		QJsonObject propNum;
		propNum[QStringLiteral("type")] = QStringLiteral("integer");
		propNum[QStringLiteral("description")] = QStringLiteral("Time signature numerator (beats per bar, e.g. 4, 3, 6, 7).");
		props[QStringLiteral("time_signature_num")] = propNum;

		QJsonObject propDenom;
		propDenom[QStringLiteral("type")] = QStringLiteral("integer");
		propDenom[QStringLiteral("description")] = QStringLiteral("Time signature denominator (beat unit, e.g. 4 for quarter-notes, 8 for eighth-notes).");
		props[QStringLiteral("time_signature_denom")] = propDenom;

		reg.registerTool(
			QStringLiteral("set_tempo"),
			QStringLiteral("Sets project BPM tempo and optional time signature numerator and denominator."),
			props,
			QJsonArray(),
			handleSetTempo
		);
	}

	// 10. export_audio
	{
		QJsonObject props;
		QJsonObject propOut;
		propOut[QStringLiteral("type")] = QStringLiteral("string");
		propOut[QStringLiteral("description")] = QStringLiteral("Target audio file path (e.g. 'rendered_track.wav'). Automatically created in user projects dir if relative.");
		props[QStringLiteral("output_file")] = propOut;

		QJsonObject propFmt;
		propFmt[QStringLiteral("type")] = QStringLiteral("string");
		propFmt[QStringLiteral("description")] = QStringLiteral("Audio format: 'wav', 'mp3', 'ogg', or 'flac' (default: 'wav').");
		props[QStringLiteral("format")] = propFmt;

		QJsonObject propSr;
		propSr[QStringLiteral("type")] = QStringLiteral("integer");
		propSr[QStringLiteral("description")] = QStringLiteral("Sample rate in Hz (e.g. 44100, 48000, 96000, default: 44100).");
		props[QStringLiteral("samplerate")] = propSr;

		QJsonObject propBd;
		propBd[QStringLiteral("type")] = QStringLiteral("integer");
		propBd[QStringLiteral("description")] = QStringLiteral("Bit depth: 16, 24, or 32 (default: 16).");
		props[QStringLiteral("bit_depth")] = propBd;

		QJsonArray req;
		req.append(QStringLiteral("output_file"));

		reg.registerTool(
			QStringLiteral("export_audio"),
			QStringLiteral("Renders the finished LMMS project directly to disk as a WAV, MP3, OGG, or FLAC audio file."),
			props,
			req,
			handleExportAudio
		);
	}

	// 11. add_timeline_marker
	{
		QJsonObject props;
		QJsonObject propBar;
		propBar[QStringLiteral("type")] = QStringLiteral("number");
		propBar[QStringLiteral("description")] = QStringLiteral("Timeline bar position where the marker should be placed (0-indexed).");
		props[QStringLiteral("bar")] = propBar;

		QJsonObject propName;
		propName[QStringLiteral("type")] = QStringLiteral("string");
		propName[QStringLiteral("description")] = QStringLiteral("Descriptive name/label for the song section (e.g. 'Intro', 'Verse 1', 'Chorus', 'Build', 'Drop', 'Outro').");
		props[QStringLiteral("name")] = propName;

		QJsonArray req;
		req.append(QStringLiteral("bar"));
		req.append(QStringLiteral("name"));

		reg.registerTool(
			QStringLiteral("add_timeline_marker"),
			QStringLiteral("Adds a labeled arrangement marker onto the song timeline ('Intro', 'Chorus', 'Drop', etc.) to enable structured composition."),
			props,
			req,
			handleAddTimelineMarker
		);
	}

	// 12. get_timeline_markers
	{
		reg.registerTool(
			QStringLiteral("get_timeline_markers"),
			QStringLiteral("Returns all labeled timeline arrangement markers in the project."),
			QJsonObject(),
			QJsonArray(),
			handleGetTimelineMarkers
		);
	}

	// 13. import_midi
	{
		QJsonObject props;
		QJsonObject propFile;
		propFile[QStringLiteral("type")] = QStringLiteral("string");
		propFile[QStringLiteral("description")] = QStringLiteral("Path to standard MIDI file (.mid, .midi) on disk.");
		props[QStringLiteral("file_path")] = propFile;

		QJsonArray req;
		req.append(QStringLiteral("file_path"));

		reg.registerTool(
			QStringLiteral("import_midi"),
			QStringLiteral("Imports standard .mid/.midi files into the current LMMS song project using LMMS MIDI import filters."),
			props,
			req,
			handleImportMidi
		);
	}

	// 14. export_midi
	{
		QJsonObject props;
		QJsonObject propFile;
		propFile[QStringLiteral("type")] = QStringLiteral("string");
		propFile[QStringLiteral("description")] = QStringLiteral("Path to save the exported standard MIDI (.mid) file.");
		props[QStringLiteral("file_path")] = propFile;

		QJsonArray req;
		req.append(QStringLiteral("file_path"));

		reg.registerTool(
			QStringLiteral("export_midi"),
			QStringLiteral("Exports all timeline tracks and notes to a Standard MIDI file (.mid) for use in other DAWs or external tools."),
			props,
			req,
			handleExportMidi
		);
	}
}

} // namespace lmms
