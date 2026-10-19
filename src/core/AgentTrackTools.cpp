/*
 * AgentTrackTools.cpp - Track, Clip, and Note tools for AI Agent in LMMS
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

#include "AgentTrackTools.h"
#include "AgentToolRegistry.h"

#include "Engine.h"
#include "Song.h"
#include "Track.h"
#include "SampleTrack.h"
#include "SampleClip.h"
#include "InstrumentTrack.h"
#include "AutomationTrack.h"
#include "AutomationClip.h"
#include "AutomationNode.h"
#include "PatternStore.h"
#include "Mixer.h"
#include "TimePos.h"
#include "Clip.h"
#include "Note.h"
#include "MidiClip.h"
#include "ConfigManager.h"
#include "Instrument.h"

#include <QFileInfo>
#include <QFile>
#include <algorithm>

namespace lmms
{

static int parseKeyStringOrInt(const QJsonValue& val, bool& ok)
{
	ok = true;
	if (val.isDouble())
	{
		int k = val.toInt();
		if (k < 0) k = 0;
		if (k > 127) k = 127;
		return k;
	}

	QString str = val.toString().trimmed().toUpper();
	if (str.isEmpty())
	{
		ok = false;
		return 60; // Default C4
	}

	bool isNum = false;
	int num = str.toInt(&isNum);
	if (isNum)
	{
		if (num < 0) num = 0;
		if (num > 127) num = 127;
		return num;
	}

	int idx = 0;
	QChar letter = str.at(idx++);
	int semitone = 0;
	switch (letter.toLatin1())
	{
		case 'C': semitone = 0; break;
		case 'D': semitone = 2; break;
		case 'E': semitone = 4; break;
		case 'F': semitone = 5; break;
		case 'G': semitone = 7; break;
		case 'A': semitone = 9; break;
		case 'B': semitone = 11; break;
		default:
			ok = false;
			return 60;
	}

	if (idx < str.length())
	{
		if (str.at(idx) == '#')
		{
			semitone += 1;
			idx++;
		}
		else if (str.at(idx) == 'B' && idx + 1 < str.length())
		{
			semitone -= 1;
			idx++;
		}
	}

	int octave = 4;
	if (idx < str.length())
	{
		QString octStr = str.mid(idx);
		bool octOk = false;
		int octVal = octStr.toInt(&octOk);
		if (octOk)
		{
			octave = octVal;
		}
	}

	int key = (octave + 1) * 12 + semitone;
	if (key < 0) key = 0;
	if (key > 127) key = 127;
	return key;
}

static QJsonObject handleAddSampleClip(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	QString samplePath = args.value(QStringLiteral("sample_path")).toString().trimmed();
	if (samplePath.isEmpty())
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameter: 'sample_path'.");
		return QJsonObject();
	}

	QFileInfo fi(samplePath);
	if (!fi.exists())
	{
		QString candidate = ConfigManager::inst()->userSamplesDir() + samplePath;
		if (QFile::exists(candidate))
		{
			samplePath = candidate;
			fi.setFile(samplePath);
		}
		else
		{
			candidate = ConfigManager::inst()->workingDir() + samplePath;
			if (QFile::exists(candidate))
			{
				samplePath = candidate;
				fi.setFile(samplePath);
			}
			else
			{
				ok = false;
				errorMsg = QStringLiteral("Sample file does not exist: %1").arg(samplePath);
				return QJsonObject();
			}
		}
	}
	samplePath = fi.absoluteFilePath();

	tick_t startTick = 0;
	if (args.contains(QStringLiteral("start_bar")))
	{
		startTick = static_cast<tick_t>(args.value(QStringLiteral("start_bar")).toDouble() * TimePos::ticksPerBar());
		if (args.contains(QStringLiteral("start_tick")))
		{
			startTick += args.value(QStringLiteral("start_tick")).toInt();
		}
	}
	else if (args.contains(QStringLiteral("start_tick")))
	{
		startTick = args.value(QStringLiteral("start_tick")).toInt();
	}

	if (startTick < 0)
	{
		startTick = 0;
	}

	bool forceNewTrack = args.value(QStringLiteral("new_track")).toBool(false);
	SampleTrack* sampleTrack = nullptr;
	int targetTrackIdx = -1;

	if (!forceNewTrack && args.contains(QStringLiteral("track_index")))
	{
		int reqIdx = args.value(QStringLiteral("track_index")).toInt();
		const auto& tracks = song->tracks();
		if (reqIdx >= 0 && reqIdx < static_cast<int>(tracks.size()))
		{
			sampleTrack = dynamic_cast<SampleTrack*>(tracks[reqIdx]);
			if (sampleTrack)
			{
				targetTrackIdx = reqIdx;
			}
		}
	}

	if (!sampleTrack)
	{
		if (!forceNewTrack && !args.contains(QStringLiteral("track_index")))
		{
			const auto& tracks = song->tracks();
			for (size_t i = 0; i < tracks.size(); ++i)
			{
				sampleTrack = dynamic_cast<SampleTrack*>(tracks[i]);
				if (sampleTrack)
				{
					targetTrackIdx = static_cast<int>(i);
					break;
				}
			}
		}

		if (!sampleTrack)
		{
			Track* t = Track::create(Track::Type::Sample, song);
			sampleTrack = dynamic_cast<SampleTrack*>(t);
			if (!sampleTrack)
			{
				ok = false;
				errorMsg = QStringLiteral("Failed to create SampleTrack.");
				return QJsonObject();
			}
			sampleTrack->setName(fi.baseName());
			const auto& tracks = song->tracks();
			for (size_t i = 0; i < tracks.size(); ++i)
			{
				if (tracks[i] == sampleTrack)
				{
					targetTrackIdx = static_cast<int>(i);
					break;
				}
			}
		}
	}

	Clip* clip = sampleTrack->createClip(TimePos(startTick));
	auto sClip = dynamic_cast<SampleClip*>(clip);
	if (!sClip)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to create SampleClip on SampleTrack.");
		return QJsonObject();
	}

	sClip->setSampleFile(samplePath);

	if (args.contains(QStringLiteral("start_frame_offset")))
	{
		int offset = args.value(QStringLiteral("start_frame_offset")).toInt(0);
		if (offset > 0)
		{
			sClip->setSampleStartFrame(offset);
		}
	}

	if (args.contains(QStringLiteral("length_ticks")))
	{
		int lenTicks = args.value(QStringLiteral("length_ticks")).toInt();
		if (lenTicks > 0)
		{
			sClip->changeLength(TimePos(lenTicks));
			sClip->setAutoResize(false);
		}
	}
	else
	{
		sClip->updateLength();
	}

	song->updateLength();
	song->setModified();

	sClip->updateTrackClips();
	sampleTrack->updateClips();
	emit sClip->dataChanged();
	emit sampleTrack->dataChanged();

	int clipIndex = sampleTrack->numOfClips() - 1;

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Sample clip added successfully.");
	data[QStringLiteral("track_index")] = targetTrackIdx;
	data[QStringLiteral("clip_index")] = clipIndex;
	data[QStringLiteral("sample_path")] = samplePath;
	data[QStringLiteral("start_tick")] = static_cast<int>(sClip->startPosition().getTicks());
	data[QStringLiteral("start_bar")] = static_cast<double>(sClip->startPosition().getTicks()) / TimePos::ticksPerBar();
	data[QStringLiteral("length_ticks")] = static_cast<int>(sClip->length().getTicks());
	return data;
}

static QJsonObject handleSetClipProperties(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	if (!args.contains(QStringLiteral("track_index")) || !args.contains(QStringLiteral("clip_index")))
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameters: 'track_index' and 'clip_index'.");
		return QJsonObject();
	}

	int trackIdx = args.value(QStringLiteral("track_index")).toInt(-1);
	int clipIdx = args.value(QStringLiteral("clip_index")).toInt(-1);

	const auto& tracks = song->tracks();
	if (trackIdx < 0 || trackIdx >= static_cast<int>(tracks.size()))
	{
		ok = false;
		errorMsg = QStringLiteral("Invalid track_index %1 (total tracks: %2)").arg(trackIdx).arg(tracks.size());
		return QJsonObject();
	}

	Track* track = tracks[trackIdx];
	if (!track)
	{
		ok = false;
		errorMsg = QStringLiteral("Track at index %1 is null.").arg(trackIdx);
		return QJsonObject();
	}

	if (clipIdx < 0 || clipIdx >= track->numOfClips())
	{
		ok = false;
		errorMsg = QStringLiteral("Invalid clip_index %1 on track %2 (total clips: %3)").arg(clipIdx).arg(trackIdx).arg(track->numOfClips());
		return QJsonObject();
	}

	Clip* clip = track->getClip(clipIdx);
	if (!clip)
	{
		ok = false;
		errorMsg = QStringLiteral("Clip at index %1 is null.").arg(clipIdx);
		return QJsonObject();
	}

	if (args.contains(QStringLiteral("start_bar")) || args.contains(QStringLiteral("start_tick")))
	{
		tick_t newTick = clip->startPosition().getTicks();
		if (args.contains(QStringLiteral("start_bar")))
		{
			newTick = static_cast<tick_t>(args.value(QStringLiteral("start_bar")).toDouble() * TimePos::ticksPerBar());
			if (args.contains(QStringLiteral("start_tick")))
			{
				newTick += args.value(QStringLiteral("start_tick")).toInt();
			}
		}
		else if (args.contains(QStringLiteral("start_tick")))
		{
			newTick = args.value(QStringLiteral("start_tick")).toInt();
		}

		if (newTick < 0) newTick = 0;
		clip->movePosition(TimePos(newTick));
	}

	if (args.contains(QStringLiteral("length_ticks")))
	{
		int lenTicks = args.value(QStringLiteral("length_ticks")).toInt();
		if (lenTicks > 0)
		{
			clip->changeLength(TimePos(lenTicks));
			clip->setAutoResize(false);
		}
	}

	if (args.contains(QStringLiteral("mute")))
	{
		clip->setMuted(args.value(QStringLiteral("mute")).toBool());
	}

	if (args.contains(QStringLiteral("start_frame_offset")))
	{
		auto sClip = dynamic_cast<SampleClip*>(clip);
		if (sClip)
		{
			sClip->setSampleStartFrame(args.value(QStringLiteral("start_frame_offset")).toInt(0));
		}
	}

	song->updateLength();
	song->setModified();
	emit clip->dataChanged();
	emit track->dataChanged();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Clip properties updated successfully.");
	data[QStringLiteral("track_index")] = trackIdx;
	data[QStringLiteral("clip_index")] = clipIdx;
	data[QStringLiteral("start_tick")] = static_cast<int>(clip->startPosition().getTicks());
	data[QStringLiteral("start_bar")] = static_cast<double>(clip->startPosition().getTicks()) / TimePos::ticksPerBar();
	data[QStringLiteral("length_ticks")] = static_cast<int>(clip->length().getTicks());
	data[QStringLiteral("is_muted")] = clip->isMuted();
	return data;
}

static QJsonObject handleAddNotes(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	if (!args.contains(QStringLiteral("track_index")) || !args.contains(QStringLiteral("notes")))
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameters: 'track_index' and 'notes'.");
		return QJsonObject();
	}

	int trackIdx = args.value(QStringLiteral("track_index")).toInt(-1);
	const auto& tracks = song->tracks();
	if (trackIdx < 0 || trackIdx >= static_cast<int>(tracks.size()))
	{
		ok = false;
		errorMsg = QStringLiteral("Invalid track_index %1 (total tracks: %2).").arg(trackIdx).arg(tracks.size());
		return QJsonObject();
	}

	auto it = dynamic_cast<InstrumentTrack*>(tracks[trackIdx]);
	if (!it)
	{
		ok = false;
		errorMsg = QStringLiteral("Track at index %1 is not an InstrumentTrack (type: %2). Notes can only be added to InstrumentTracks.")
			.arg(trackIdx).arg(static_cast<int>(tracks[trackIdx]->type()));
		return QJsonObject();
	}

	QJsonArray notesArray = args.value(QStringLiteral("notes")).toArray();
	if (notesArray.isEmpty())
	{
		ok = false;
		errorMsg = QStringLiteral("'notes' array must contain at least one note.");
		return QJsonObject();
	}

	MidiClip* midiClip = nullptr;
	int targetClipIdx = -1;

	if (args.contains(QStringLiteral("clip_index")))
	{
		int reqClipIdx = args.value(QStringLiteral("clip_index")).toInt(-1);
		if (reqClipIdx >= 0 && reqClipIdx < it->numOfClips())
		{
			midiClip = dynamic_cast<MidiClip*>(it->getClip(reqClipIdx));
			if (midiClip)
			{
				targetClipIdx = reqClipIdx;
			}
		}
	}

	if (!midiClip)
	{
		tick_t startTick = 0;
		if (args.contains(QStringLiteral("start_bar")))
		{
			startTick = static_cast<tick_t>(args.value(QStringLiteral("start_bar")).toDouble() * TimePos::ticksPerBar());
			if (args.contains(QStringLiteral("start_tick")))
			{
				startTick += args.value(QStringLiteral("start_tick")).toInt();
			}
		}
		else if (args.contains(QStringLiteral("start_tick")))
		{
			startTick = args.value(QStringLiteral("start_tick")).toInt();
		}

		if (startTick < 0) startTick = 0;

		Clip* clip = it->createClip(TimePos(startTick));
		midiClip = dynamic_cast<MidiClip*>(clip);
		if (!midiClip)
		{
			ok = false;
			errorMsg = QStringLiteral("Failed to create MidiClip on track %1.").arg(trackIdx);
			return QJsonObject();
		}
		targetClipIdx = it->numOfClips() - 1;
	}

	int notesAdded = 0;
	for (const QJsonValue& nv : notesArray)
	{
		if (!nv.isObject()) continue;
		QJsonObject no = nv.toObject();

		bool keyOk = false;
		int keyNum = parseKeyStringOrInt(no.value(QStringLiteral("key")), keyOk);
		if (!keyOk)
		{
			continue;
		}

		tick_t posTicks = 0;
		if (no.contains(QStringLiteral("pos_tick")))
		{
			posTicks = no.value(QStringLiteral("pos_tick")).toInt(0);
		}
		else if (no.contains(QStringLiteral("pos_beat")))
		{
			posTicks = static_cast<tick_t>(no.value(QStringLiteral("pos_beat")).toDouble(0.0) * 48.0);
		}

		if (posTicks < 0) posTicks = 0;

		tick_t lenTicks = 48;
		if (no.contains(QStringLiteral("len_tick")))
		{
			lenTicks = no.value(QStringLiteral("len_tick")).toInt(48);
		}
		else if (no.contains(QStringLiteral("len_beat")))
		{
			lenTicks = static_cast<tick_t>(no.value(QStringLiteral("len_beat")).toDouble(1.0) * 48.0);
		}

		if (lenTicks <= 0) lenTicks = 1;

		volume_t vol = static_cast<volume_t>(no.value(QStringLiteral("volume")).toInt(100));
		panning_t pan = static_cast<panning_t>(no.value(QStringLiteral("panning")).toInt(0));

		Note n(TimePos(lenTicks), TimePos(posTicks), keyNum, vol, pan);
		midiClip->addNote(n, false);
		notesAdded++;
	}

	midiClip->updateLength();
	song->updateLength();
	song->setModified();

	emit midiClip->dataChanged();
	emit it->dataChanged();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Successfully added %1 notes to track %2 (clip %3).")
		.arg(notesAdded).arg(trackIdx).arg(targetClipIdx);
	data[QStringLiteral("track_index")] = trackIdx;
	data[QStringLiteral("clip_index")] = targetClipIdx;
	data[QStringLiteral("notes_count")] = notesAdded;
	data[QStringLiteral("clip_start_tick")] = static_cast<int>(midiClip->startPosition().getTicks());
	data[QStringLiteral("clip_length_ticks")] = static_cast<int>(midiClip->length().getTicks());
	return data;
}

static QJsonObject handleAddTrack(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	QString typeStr = args.value(QStringLiteral("type")).toString().trimmed().toLower();
	if (typeStr.isEmpty())
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameter: 'type'. (Expected 'instrument', 'sample', 'pattern', or 'automation').");
		return QJsonObject();
	}

	Track::Type trackType = Track::Type::Instrument;
	if (typeStr == QStringLiteral("instrument"))
	{
		trackType = Track::Type::Instrument;
	}
	else if (typeStr == QStringLiteral("sample"))
	{
		trackType = Track::Type::Sample;
	}
	else if (typeStr == QStringLiteral("pattern") || typeStr == QStringLiteral("beat"))
	{
		trackType = Track::Type::Pattern;
	}
	else if (typeStr == QStringLiteral("automation"))
	{
		trackType = Track::Type::Automation;
	}
	else
	{
		ok = false;
		errorMsg = QStringLiteral("Unsupported track type: '%1'. Expected 'instrument', 'sample', 'pattern', or 'automation'.").arg(typeStr);
		return QJsonObject();
	}

	Track* t = Track::create(trackType, song);
	if (!t)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to create track of type '%1'.").arg(typeStr);
		return QJsonObject();
	}

	QString customName = args.value(QStringLiteral("name")).toString().trimmed();
	if (!customName.isEmpty())
	{
		t->setName(customName);
	}

	song->setModified();
	emit t->dataChanged();

	int newTrackIdx = -1;
	const auto& tracks = song->tracks();
	for (size_t i = 0; i < tracks.size(); ++i)
	{
		if (tracks[i] == t)
		{
			newTrackIdx = static_cast<int>(i);
			break;
		}
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Track created successfully.");
	data[QStringLiteral("track_index")] = newTrackIdx;
	data[QStringLiteral("type")] = typeStr;
	data[QStringLiteral("name")] = t->name();
	return data;
}

static QString midiKeyToNoteName(int key)
{
	static const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
	int octave = (key / 12) - 1;
	int noteInOctave = key % 12;
	if (noteInOctave < 0) noteInOctave += 12;
	return QStringLiteral("%1%2").arg(QString::fromLatin1(noteNames[noteInOctave])).arg(octave);
}

static QJsonObject handleGetClipNotes(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	int trackIndex = args.value(QStringLiteral("track_index")).toInt(-1);
	int clipIndex = args.value(QStringLiteral("clip_index")).toInt(-1);

	if (trackIndex < 0 || trackIndex >= static_cast<int>(song->tracks().size()))
	{
		ok = false;
		errorMsg = QStringLiteral("Track index %1 is out of range.").arg(trackIndex);
		return QJsonObject();
	}

	Track* t = song->tracks()[trackIndex];
	if (clipIndex < 0 || clipIndex >= t->numOfClips())
	{
		ok = false;
		errorMsg = QStringLiteral("Clip index %1 is out of range for track '%2' (has %3 clips).").arg(clipIndex).arg(t->name()).arg(t->numOfClips());
		return QJsonObject();
	}

	Clip* c = t->getClip(clipIndex);
	auto mc = dynamic_cast<MidiClip*>(c);
	if (!mc)
	{
		ok = false;
		errorMsg = QStringLiteral("Clip at index %1 on track '%2' is not a piano roll MidiClip.").arg(clipIndex).arg(t->name());
		return QJsonObject();
	}

	QJsonArray notesArr;
	for (const Note* n : mc->notes())
	{
		if (!n) continue;
		QJsonObject no;
		no[QStringLiteral("key")] = n->key();
		no[QStringLiteral("name")] = midiKeyToNoteName(n->key());
		no[QStringLiteral("pos_tick")] = n->pos().getTicks();
		no[QStringLiteral("pos_beat")] = static_cast<double>(n->pos().getTicks()) / (DefaultTicksPerBar / 4.0);
		no[QStringLiteral("len_tick")] = n->length().getTicks();
		no[QStringLiteral("len_beat")] = static_cast<double>(n->length().getTicks()) / (DefaultTicksPerBar / 4.0);
		no[QStringLiteral("volume")] = n->getVolume();
		no[QStringLiteral("panning")] = n->getPanning();
		notesArr.append(no);
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("track_index")] = trackIndex;
	data[QStringLiteral("clip_index")] = clipIndex;
	data[QStringLiteral("start_bar")] = static_cast<double>(mc->startPosition().getTicks()) / song->ticksPerBar();
	data[QStringLiteral("start_tick")] = mc->startPosition().getTicks();
	data[QStringLiteral("length_ticks")] = mc->length().getTicks();
	data[QStringLiteral("note_count")] = notesArr.size();
	data[QStringLiteral("notes")] = notesArr;
	return data;
}

static QJsonObject handleClearNotes(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	int trackIndex = args.value(QStringLiteral("track_index")).toInt(-1);
	int clipIndex = args.value(QStringLiteral("clip_index")).toInt(-1);

	if (trackIndex < 0 || trackIndex >= static_cast<int>(song->tracks().size()))
	{
		ok = false;
		errorMsg = QStringLiteral("Track index %1 is out of range.").arg(trackIndex);
		return QJsonObject();
	}

	Track* t = song->tracks()[trackIndex];
	if (clipIndex < 0 || clipIndex >= t->numOfClips())
	{
		ok = false;
		errorMsg = QStringLiteral("Clip index %1 is out of range for track '%2'.").arg(clipIndex).arg(t->name());
		return QJsonObject();
	}

	Clip* c = t->getClip(clipIndex);
	auto mc = dynamic_cast<MidiClip*>(c);
	if (!mc)
	{
		ok = false;
		errorMsg = QStringLiteral("Clip at index %1 on track '%2' is not a piano roll MidiClip.").arg(clipIndex).arg(t->name());
		return QJsonObject();
	}

	int clearedCount = static_cast<int>(mc->notes().size());
	mc->clearNotes();
	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Cleared all notes from clip.");
	data[QStringLiteral("track_index")] = trackIndex;
	data[QStringLiteral("clip_index")] = clipIndex;
	data[QStringLiteral("cleared_note_count")] = clearedCount;
	return data;
}

static QJsonObject handleDeleteClip(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	int trackIndex = args.value(QStringLiteral("track_index")).toInt(-1);
	int clipIndex = args.value(QStringLiteral("clip_index")).toInt(-1);

	if (trackIndex < 0 || trackIndex >= static_cast<int>(song->tracks().size()))
	{
		ok = false;
		errorMsg = QStringLiteral("Track index %1 is out of range.").arg(trackIndex);
		return QJsonObject();
	}

	Track* t = song->tracks()[trackIndex];
	if (clipIndex < 0 || clipIndex >= t->numOfClips())
	{
		ok = false;
		errorMsg = QStringLiteral("Clip index %1 is out of range for track '%2'.").arg(clipIndex).arg(t->name());
		return QJsonObject();
	}

	Clip* c = t->getClip(clipIndex);
	delete c;

	song->updateLength();
	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Clip deleted successfully.");
	data[QStringLiteral("track_index")] = trackIndex;
	data[QStringLiteral("deleted_clip_index")] = clipIndex;
	data[QStringLiteral("remaining_clip_count")] = t->numOfClips();
	return data;
}

static Track* resolveBBTrack(int trackIndex, bool inBBEditor, Song* song)
{
	if (inBBEditor && Engine::patternStore() && !Engine::patternStore()->tracks().empty())
	{
		if (trackIndex >= 0 && trackIndex < static_cast<int>(Engine::patternStore()->tracks().size()))
		{
			return Engine::patternStore()->tracks()[trackIndex];
		}
	}

	if (trackIndex >= 0 && trackIndex < static_cast<int>(song->tracks().size()))
	{
		return song->tracks()[trackIndex];
	}

	return nullptr;
}

static QJsonObject handleSetStepState(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	int trackIndex = args.value(QStringLiteral("track_index")).toInt(-1);
	int stepIndex = args.value(QStringLiteral("step_index")).toInt(-1);
	bool active = args.value(QStringLiteral("active")).toBool(true);
	int patternIndex = args.value(QStringLiteral("pattern_index")).toInt(0);
	bool inBBEditor = args.value(QStringLiteral("in_bb_editor")).toBool(true);

	if (stepIndex < 0)
	{
		ok = false;
		errorMsg = QStringLiteral("step_index must be >= 0.");
		return QJsonObject();
	}

	Track* t = resolveBBTrack(trackIndex, inBBEditor, song);
	if (!t)
	{
		ok = false;
		errorMsg = QStringLiteral("Track index %1 not found in %2.")
			.arg(trackIndex)
			.arg(inBBEditor ? QStringLiteral("Beat+Bassline editor") : QStringLiteral("Song editor"));
		return QJsonObject();
	}

	auto it = dynamic_cast<InstrumentTrack*>(t);
	if (!it)
	{
		ok = false;
		errorMsg = QStringLiteral("Track '%1' is not an InstrumentTrack and does not support steps.").arg(t->name());
		return QJsonObject();
	}

	Clip* c = it->getClip(patternIndex);
	auto mc = dynamic_cast<MidiClip*>(c);
	if (!mc)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to access MidiClip for pattern %1 on track '%2'.").arg(patternIndex).arg(t->name());
		return QJsonObject();
	}

	mc->setStep(stepIndex, active);
	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Step state updated.");
	data[QStringLiteral("track_index")] = trackIndex;
	data[QStringLiteral("track_name")] = t->name();
	data[QStringLiteral("step_index")] = stepIndex;
	data[QStringLiteral("active")] = active;
	data[QStringLiteral("pattern_index")] = patternIndex;
	return data;
}

static QJsonObject handleControlBBPattern(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Song* song = Engine::getSong();
	if (!song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Song engine is not available.");
		return QJsonObject();
	}

	int trackIndex = args.value(QStringLiteral("track_index")).toInt(-1);
	int patternIndex = args.value(QStringLiteral("pattern_index")).toInt(0);
	bool inBBEditor = args.value(QStringLiteral("in_bb_editor")).toBool(true);
	bool clearFirst = args.value(QStringLiteral("clear_first")).toBool(false);

	Track* t = resolveBBTrack(trackIndex, inBBEditor, song);
	if (!t)
	{
		ok = false;
		errorMsg = QStringLiteral("Track index %1 not found in %2.")
			.arg(trackIndex)
			.arg(inBBEditor ? QStringLiteral("Beat+Bassline editor") : QStringLiteral("Song editor"));
		return QJsonObject();
	}

	auto it = dynamic_cast<InstrumentTrack*>(t);
	if (!it)
	{
		ok = false;
		errorMsg = QStringLiteral("Track '%1' is not an InstrumentTrack.").arg(t->name());
		return QJsonObject();
	}

	Clip* c = it->getClip(patternIndex);
	auto mc = dynamic_cast<MidiClip*>(c);
	if (!mc)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to access MidiClip for pattern %1 on track '%2'.").arg(patternIndex).arg(t->name());
		return QJsonObject();
	}

	if (clearFirst)
	{
		mc->clearNotes();
	}

	int stepsConfigured = 0;

	if (args.contains(QStringLiteral("steps")))
	{
		QJsonValue stepsVal = args.value(QStringLiteral("steps"));
		if (stepsVal.isArray())
		{
			QJsonArray arr = stepsVal.toArray();
			for (int i = 0; i < arr.size(); ++i)
			{
				bool active = false;
				if (arr[i].isBool()) active = arr[i].toBool();
				else if (arr[i].isDouble()) active = (arr[i].toInt() != 0);
				mc->setStep(i, active);
				stepsConfigured++;
			}
		}
		else if (stepsVal.isString())
		{
			QString patternStr = stepsVal.toString().trimmed();
			for (int i = 0; i < patternStr.length(); ++i)
			{
				QChar ch = patternStr.at(i);
				bool active = (ch == 'x' || ch == 'X' || ch == '1' || ch == '*' || ch == 'o' || ch == 'O');
				mc->setStep(i, active);
				stepsConfigured++;
			}
		}
	}

	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Beat & Bassline pattern programmed successfully.");
	data[QStringLiteral("track_index")] = trackIndex;
	data[QStringLiteral("track_name")] = t->name();
	data[QStringLiteral("pattern_index")] = patternIndex;
	data[QStringLiteral("steps_configured")] = stepsConfigured;
	return data;
}

static QJsonObject handleAddAutomationNode(const QJsonObject& args, bool& ok, QString& errorMsg)
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
	auto at = dynamic_cast<AutomationTrack*>(t);
	if (!at)
	{
		ok = false;
		errorMsg = QStringLiteral("Track at index %1 ('%2') is not an AutomationTrack.").arg(trackIndex).arg(t->name());
		return QJsonObject();
	}

	int clipIndex = args.value(QStringLiteral("clip_index")).toInt(0);
	AutomationClip* ac = nullptr;
	if (clipIndex < at->numOfClips())
	{
		ac = dynamic_cast<AutomationClip*>(at->getClip(clipIndex));
	}
	else
	{
		double startBar = args.value(QStringLiteral("start_bar")).toDouble(0.0);
		int startTick = args.value(QStringLiteral("start_tick")).toInt(0);
		tick_t pos = static_cast<tick_t>(startBar * song->ticksPerBar()) + startTick;
		ac = dynamic_cast<AutomationClip*>(at->createClip(TimePos(pos)));
	}

	if (!ac)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to find or create AutomationClip on track %1.").arg(trackIndex);
		return QJsonObject();
	}

	if (args.contains(QStringLiteral("progression_type")))
	{
		QString pt = args.value(QStringLiteral("progression_type")).toString().trimmed().toLower();
		if (pt == QStringLiteral("discrete"))
		{
			ac->setProgressionType(AutomationClip::ProgressionType::Discrete);
		}
		else if (pt == QStringLiteral("cubic") || pt == QStringLiteral("cubichermite"))
		{
			ac->setProgressionType(AutomationClip::ProgressionType::CubicHermite);
		}
		else
		{
			ac->setProgressionType(AutomationClip::ProgressionType::Linear);
		}
	}

	if (args.contains(QStringLiteral("tension")))
	{
		double tensionVal = args.value(QStringLiteral("tension")).toDouble(1.0);
		ac->setTension(QString::number(tensionVal));
	}

	int nodesAdded = 0;
	int tpb = song->ticksPerBar();

	if (args.contains(QStringLiteral("nodes")) && args.value(QStringLiteral("nodes")).isArray())
	{
		QJsonArray nodes = args.value(QStringLiteral("nodes")).toArray();
		for (const QJsonValue& nv : nodes)
		{
			if (!nv.isObject()) continue;
			QJsonObject no = nv.toObject();
			double bar = no.value(QStringLiteral("bar")).toDouble(0.0);
			int tick = no.value(QStringLiteral("tick")).toInt(0);
			float val = static_cast<float>(no.value(QStringLiteral("value")).toDouble(0.0));
			tick_t nodeTicks = static_cast<tick_t>(bar * tpb) + tick;
			if (nodeTicks < 0) nodeTicks = 0;
			ac->putValue(TimePos(nodeTicks), val, false);
			nodesAdded++;
		}
	}
	else if (args.contains(QStringLiteral("bar")) || args.contains(QStringLiteral("value")))
	{
		double bar = args.value(QStringLiteral("bar")).toDouble(0.0);
		int tick = args.value(QStringLiteral("tick")).toInt(0);
		float val = static_cast<float>(args.value(QStringLiteral("value")).toDouble(0.0));
		tick_t nodeTicks = static_cast<tick_t>(bar * tpb) + tick;
		if (nodeTicks < 0) nodeTicks = 0;
		ac->putValue(TimePos(nodeTicks), val, false);
		nodesAdded++;
	}

	song->updateLength();
	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Automation node(s) added successfully.");
	data[QStringLiteral("track_index")] = trackIndex;
	data[QStringLiteral("clip_index")] = clipIndex;
	data[QStringLiteral("nodes_added")] = nodesAdded;
	return data;
}

static QJsonObject handleLinkControl(const QJsonObject& args, bool& ok, QString& errorMsg)
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
	auto at = dynamic_cast<AutomationTrack*>(t);
	if (!at)
	{
		ok = false;
		errorMsg = QStringLiteral("Track at index %1 is not an AutomationTrack.").arg(trackIndex);
		return QJsonObject();
	}

	int clipIndex = args.value(QStringLiteral("clip_index")).toInt(0);
	AutomationClip* ac = nullptr;
	if (clipIndex < at->numOfClips())
	{
		ac = dynamic_cast<AutomationClip*>(at->getClip(clipIndex));
	}
	else
	{
		ac = dynamic_cast<AutomationClip*>(at->createClip(TimePos(0)));
	}

	if (!ac)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to find or create AutomationClip on track %1.").arg(trackIndex);
		return QJsonObject();
	}

	QString targetType = args.value(QStringLiteral("target_type")).toString().trimmed().toLower();
	AutomatableModel* targetModel = nullptr;

	if (targetType == QStringLiteral("tempo"))
	{
		targetModel = &song->tempoModel();
	}
	else if (targetType == QStringLiteral("master_volume") || targetType == QStringLiteral("master_pitch"))
	{
		QList<AutomatableModel*> songModels = song->findChildren<AutomatableModel*>();
		for (AutomatableModel* m : songModels)
		{
			if (!m) continue;
			if (targetType == QStringLiteral("master_volume") && m->displayName().contains(QStringLiteral("Volume"), Qt::CaseInsensitive))
			{
				targetModel = m;
				break;
			}
			if (targetType == QStringLiteral("master_pitch") && m->displayName().contains(QStringLiteral("Pitch"), Qt::CaseInsensitive))
			{
				targetModel = m;
				break;
			}
		}
	}
	else if (targetType == QStringLiteral("track_volume") || targetType == QStringLiteral("track_panning") || targetType == QStringLiteral("track_pitch"))
	{
		int targetTrackIdx = args.value(QStringLiteral("target_track_index")).toInt(-1);
		if (targetTrackIdx < 0 || targetTrackIdx >= static_cast<int>(song->tracks().size()))
		{
			ok = false;
			errorMsg = QStringLiteral("target_track_index %1 is out of range.").arg(targetTrackIdx);
			return QJsonObject();
		}
		Track* targetTrk = song->tracks()[targetTrackIdx];
		if (auto it = dynamic_cast<InstrumentTrack*>(targetTrk))
		{
			if (targetType == QStringLiteral("track_volume")) targetModel = it->volumeModel();
			else if (targetType == QStringLiteral("track_panning")) targetModel = it->panningModel();
			else if (targetType == QStringLiteral("track_pitch")) targetModel = it->pitchModel();
		}
		else
		{
			ok = false;
			errorMsg = QStringLiteral("Target track %1 is not an InstrumentTrack.").arg(targetTrackIdx);
			return QJsonObject();
		}
	}
	else if (targetType == QStringLiteral("mixer_volume") || targetType == QStringLiteral("mixer_channel_volume"))
	{
		int chIdx = args.value(QStringLiteral("mixer_channel")).toInt(0);
		if (!Engine::mixer() || chIdx < 0 || chIdx >= static_cast<int>(Engine::mixer()->numChannels()))
		{
			ok = false;
			errorMsg = QStringLiteral("Mixer channel %1 is out of range.").arg(chIdx);
			return QJsonObject();
		}
		targetModel = &Engine::mixer()->mixerChannel(chIdx)->m_volumeModel;
	}
	else if (targetType == QStringLiteral("instrument_param"))
	{
		int targetTrackIdx = args.value(QStringLiteral("target_track_index")).toInt(-1);
		if (targetTrackIdx < 0 || targetTrackIdx >= static_cast<int>(song->tracks().size()))
		{
			ok = false;
			errorMsg = QStringLiteral("target_track_index %1 is out of range.").arg(targetTrackIdx);
			return QJsonObject();
		}
		Track* targetTrk = song->tracks()[targetTrackIdx];
		auto it = dynamic_cast<InstrumentTrack*>(targetTrk);
		if (!it)
		{
			ok = false;
			errorMsg = QStringLiteral("Target track %1 is not an InstrumentTrack.").arg(targetTrackIdx);
			return QJsonObject();
		}

		QString paramName = args.value(QStringLiteral("param_name")).toString().trimmed().toLower();
		paramName.remove(QLatin1Char('_')).remove(QLatin1Char(' '));

		QList<AutomatableModel*> models = it->findChildren<AutomatableModel*>();
		if (it->instrument())
		{
			models.append(it->instrument()->findChildren<AutomatableModel*>());
		}

		for (AutomatableModel* m : models)
		{
			if (!m) continue;
			QString mName = m->displayName().toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' '));
			QString mObjName = m->objectName().toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' '));
			if (mName == paramName || mObjName == paramName || mName.contains(paramName) || mObjName.contains(paramName))
			{
				targetModel = m;
				break;
			}
		}

		if (!targetModel)
		{
			ok = false;
			errorMsg = QStringLiteral("Could not find instrument parameter matching '%1'.").arg(args.value(QStringLiteral("param_name")).toString());
			return QJsonObject();
		}
	}
	else
	{
		ok = false;
		errorMsg = QStringLiteral("Unsupported target_type '%1'. Supported: 'tempo', 'master_volume', 'track_volume', 'track_panning', 'track_pitch', 'mixer_volume', 'instrument_param'.").arg(targetType);
		return QJsonObject();
	}

	if (!targetModel)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to locate target control model for '%1'.").arg(targetType);
		return QJsonObject();
	}

	bool added = ac->addObject(targetModel);
	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Control linked to automation clip successfully.");
	data[QStringLiteral("track_index")] = trackIndex;
	data[QStringLiteral("clip_index")] = clipIndex;
	data[QStringLiteral("target_type")] = targetType;
	data[QStringLiteral("model_name")] = targetModel->displayName();
	data[QStringLiteral("already_linked")] = !added;
	return data;
}

static QJsonObject handleDeleteTrack(const QJsonObject& args, bool& ok, QString& errorMsg)
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
	QString name = t->name();
	delete t;

	song->updateLength();
	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Track '%1' at index %2 deleted successfully.").arg(name).arg(trackIndex);
	data[QStringLiteral("deleted_track_name")] = name;
	data[QStringLiteral("remaining_track_count")] = static_cast<int>(song->tracks().size());
	return data;
}

static QJsonObject handleCloneTrack(const QJsonObject& args, bool& ok, QString& errorMsg)
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

	Track* orig = song->tracks()[trackIndex];
	Track* cloned = orig->clone();
	if (!cloned)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to clone track at index %1.").arg(trackIndex);
		return QJsonObject();
	}

	song->updateLength();
	song->setModified();

	int newIndex = static_cast<int>(song->tracks().size()) - 1;

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Track cloned successfully.");
	data[QStringLiteral("cloned_from_index")] = trackIndex;
	data[QStringLiteral("new_track_index")] = newIndex;
	data[QStringLiteral("name")] = cloned->name();
	data[QStringLiteral("type")] = cloned->displayName();
	return data;
}

static QJsonObject handleControlTrack(const QJsonObject& args, bool& ok, QString& errorMsg)
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

	if (args.contains(QStringLiteral("name")))
	{
		QString name = args.value(QStringLiteral("name")).toString().trimmed();
		if (!name.isEmpty())
		{
			t->setName(name);
		}
	}

	if (args.contains(QStringLiteral("muted")))
	{
		t->setMuted(args.value(QStringLiteral("muted")).toBool());
	}

	if (args.contains(QStringLiteral("solo")))
	{
		t->setSolo(args.value(QStringLiteral("solo")).toBool());
	}

	if (auto it = dynamic_cast<InstrumentTrack*>(t))
	{
		if (args.contains(QStringLiteral("volume")))
		{
			float vol = static_cast<float>(args.value(QStringLiteral("volume")).toDouble(100.0));
			it->volumeModel()->setValue(std::clamp(vol, 0.0f, 200.0f));
		}
		if (args.contains(QStringLiteral("panning")))
		{
			float pan = static_cast<float>(args.value(QStringLiteral("panning")).toDouble(0.0));
			it->panningModel()->setValue(std::clamp(pan, -100.0f, 100.0f));
		}
		if (args.contains(QStringLiteral("pitch")))
		{
			int pitchVal = args.value(QStringLiteral("pitch")).toInt(0);
			it->pitchModel()->setValue(pitchVal);
		}
		if (args.contains(QStringLiteral("mixer_channel")))
		{
			int ch = args.value(QStringLiteral("mixer_channel")).toInt(0);
			it->mixerChannelModel()->setValue(ch);
		}
	}
	else if (auto st = dynamic_cast<SampleTrack*>(t))
	{
		if (args.contains(QStringLiteral("mixer_channel")))
		{
			int ch = args.value(QStringLiteral("mixer_channel")).toInt(0);
			st->mixerChannelModel()->setValue(ch);
		}
	}

	song->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Track properties updated successfully.");
	data[QStringLiteral("track_index")] = trackIndex;
	data[QStringLiteral("name")] = t->name();
	data[QStringLiteral("type")] = t->displayName();
	data[QStringLiteral("muted")] = t->isMuted();
	data[QStringLiteral("solo")] = t->isSolo();

	if (auto it = dynamic_cast<InstrumentTrack*>(t))
	{
		data[QStringLiteral("volume")] = it->volumeModel()->value();
		data[QStringLiteral("panning")] = it->panningModel()->value();
		data[QStringLiteral("pitch")] = it->pitchModel()->value();
		data[QStringLiteral("mixer_channel")] = it->mixerChannelModel()->value();
	}
	else if (auto st = dynamic_cast<SampleTrack*>(t))
	{
		data[QStringLiteral("mixer_channel")] = st->mixerChannelModel()->value();
	}

	return data;
}

void registerTrackTools(AgentToolRegistry& reg)
{
	// 1. add_sample_clip
	{
		QJsonObject props;
		QJsonObject propSample;
		propSample[QStringLiteral("type")] = QStringLiteral("string");
		propSample[QStringLiteral("description")] = QStringLiteral("Absolute path to the audio file (.wav, .ogg, .mp3, .flac).");
		props[QStringLiteral("sample_path")] = propSample;

		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of target sample track (0-indexed). If omitted or -1, uses existing or creates new.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propNewTrack;
		propNewTrack[QStringLiteral("type")] = QStringLiteral("boolean");
		propNewTrack[QStringLiteral("description")] = QStringLiteral("If true, always creates a new SampleTrack dedicated to this clip (default: false).");
		props[QStringLiteral("new_track")] = propNewTrack;

		QJsonObject propBar;
		propBar[QStringLiteral("type")] = QStringLiteral("number");
		propBar[QStringLiteral("description")] = QStringLiteral("Timeline start bar (0-indexed, default: 0).");
		props[QStringLiteral("start_bar")] = propBar;

		QJsonObject propTick;
		propTick[QStringLiteral("type")] = QStringLiteral("integer");
		propTick[QStringLiteral("description")] = QStringLiteral("Timeline start tick offset (default: 0).");
		props[QStringLiteral("start_tick")] = propTick;

		QJsonObject propLen;
		propLen[QStringLiteral("type")] = QStringLiteral("integer");
		propLen[QStringLiteral("description")] = QStringLiteral("Explicit duration in ticks. If omitted, uses sample natural length.");
		props[QStringLiteral("length_ticks")] = propLen;

		QJsonObject propOffset;
		propOffset[QStringLiteral("type")] = QStringLiteral("integer");
		propOffset[QStringLiteral("description")] = QStringLiteral("Offset in sample audio frames to begin playback from (default: 0).");
		props[QStringLiteral("start_frame_offset")] = propOffset;

		QJsonArray required;
		required.append(QStringLiteral("sample_path"));

		reg.registerTool(
			QStringLiteral("add_sample_clip"),
			QStringLiteral("Inserts an audio sample onto the timeline at an exact position with custom playback length and timing."),
			props,
			required,
			handleAddSampleClip
		);
	}

	// 2. set_clip_properties
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the track holding the clip.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propClip;
		propClip[QStringLiteral("type")] = QStringLiteral("integer");
		propClip[QStringLiteral("description")] = QStringLiteral("Index of the clip on the track.");
		props[QStringLiteral("clip_index")] = propClip;

		QJsonObject propBar;
		propBar[QStringLiteral("type")] = QStringLiteral("number");
		propBar[QStringLiteral("description")] = QStringLiteral("New start position in bars.");
		props[QStringLiteral("start_bar")] = propBar;

		QJsonObject propTick;
		propTick[QStringLiteral("type")] = QStringLiteral("integer");
		propTick[QStringLiteral("description")] = QStringLiteral("New start position in ticks.");
		props[QStringLiteral("start_tick")] = propTick;

		QJsonObject propLen;
		propLen[QStringLiteral("type")] = QStringLiteral("integer");
		propLen[QStringLiteral("description")] = QStringLiteral("New length in ticks.");
		props[QStringLiteral("length_ticks")] = propLen;

		QJsonObject propMute;
		propMute[QStringLiteral("type")] = QStringLiteral("boolean");
		propMute[QStringLiteral("description")] = QStringLiteral("Mute or unmute the clip.");
		props[QStringLiteral("mute")] = propMute;

		QJsonObject propOffset;
		propOffset[QStringLiteral("type")] = QStringLiteral("integer");
		propOffset[QStringLiteral("description")] = QStringLiteral("Start frame offset for SampleClips.");
		props[QStringLiteral("start_frame_offset")] = propOffset;

		QJsonArray required;
		required.append(QStringLiteral("track_index"));
		required.append(QStringLiteral("clip_index"));

		reg.registerTool(
			QStringLiteral("set_clip_properties"),
			QStringLiteral("Adjusts properties of an existing clip on the timeline."),
			props,
			required,
			handleSetClipProperties
		);
	}

	// 3. add_notes
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the instrument track to add notes to.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propClip;
		propClip[QStringLiteral("type")] = QStringLiteral("integer");
		propClip[QStringLiteral("description")] = QStringLiteral("Optional index of an existing clip on the track to append notes into. If omitted, creates a new clip at start_bar/start_tick.");
		props[QStringLiteral("clip_index")] = propClip;

		QJsonObject propBar;
		propBar[QStringLiteral("type")] = QStringLiteral("number");
		propBar[QStringLiteral("description")] = QStringLiteral("Timeline position in bars where the clip sits (default: 0). 0-indexed.");
		props[QStringLiteral("start_bar")] = propBar;

		QJsonObject propTick;
		propTick[QStringLiteral("type")] = QStringLiteral("integer");
		propTick[QStringLiteral("description")] = QStringLiteral("Timeline position in ticks where the clip sits (default: 0).");
		props[QStringLiteral("start_tick")] = propTick;

		QJsonObject propNotes;
		propNotes[QStringLiteral("type")] = QStringLiteral("array");
		propNotes[QStringLiteral("description")] = QStringLiteral("Array of note objects to insert. Each object contains 'key' (e.g. 'C4', 'E4', 'G#3', 'Bb2', or MIDI number 0-127), optional 'pos_beat' (or 'pos_tick'), optional 'len_beat' (or 'len_tick'), optional 'volume' (0-200), and optional 'panning' (-100 to 100).");
		props[QStringLiteral("notes")] = propNotes;

		QJsonArray required;
		required.append(QStringLiteral("track_index"));
		required.append(QStringLiteral("notes"));

		reg.registerTool(
			QStringLiteral("add_notes"),
			QStringLiteral("Creates or appends piano roll notes into an InstrumentTrack using LMMS internal MidiClip and Note engine calls."),
			props,
			required,
			handleAddNotes
		);
	}

	// 4. add_track
	{
		QJsonObject props;
		QJsonObject propType;
		propType[QStringLiteral("type")] = QStringLiteral("string");
		propType[QStringLiteral("description")] = QStringLiteral("Type of track to add: 'instrument', 'sample', 'pattern', or 'automation'.");
		props[QStringLiteral("type")] = propType;

		QJsonObject propName;
		propName[QStringLiteral("type")] = QStringLiteral("string");
		propName[QStringLiteral("description")] = QStringLiteral("Optional custom name for the new track (e.g. 'Bassline', 'Lead Synth', 'Drums').");
		props[QStringLiteral("name")] = propName;

		QJsonArray required;
		required.append(QStringLiteral("type"));

		reg.registerTool(
			QStringLiteral("add_track"),
			QStringLiteral("Adds a new track line of any type to the timeline directly in the engine."),
			props,
			required,
			handleAddTrack
		);
	}

	// 5. get_clip_notes
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the track holding the clip.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propClip;
		propClip[QStringLiteral("type")] = QStringLiteral("integer");
		propClip[QStringLiteral("description")] = QStringLiteral("Index of the clip on the track.");
		props[QStringLiteral("clip_index")] = propClip;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));
		req.append(QStringLiteral("clip_index"));

		reg.registerTool(
			QStringLiteral("get_clip_notes"),
			QStringLiteral("Reads all piano roll notes out of an existing clip to analyze harmonies, transpose melodies, or humanize velocity and timing."),
			props,
			req,
			handleGetClipNotes
		);
	}

	// 6. clear_notes
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the track holding the clip.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propClip;
		propClip[QStringLiteral("type")] = QStringLiteral("integer");
		propClip[QStringLiteral("description")] = QStringLiteral("Index of the clip on the track.");
		props[QStringLiteral("clip_index")] = propClip;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));
		req.append(QStringLiteral("clip_index"));

		reg.registerTool(
			QStringLiteral("clear_notes"),
			QStringLiteral("Clears all notes from a piano roll clip without deleting the clip itself."),
			props,
			req,
			handleClearNotes
		);
	}

	// 7. delete_clip
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the track holding the clip.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propClip;
		propClip[QStringLiteral("type")] = QStringLiteral("integer");
		propClip[QStringLiteral("description")] = QStringLiteral("Index of the clip to delete.");
		props[QStringLiteral("clip_index")] = propClip;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));
		req.append(QStringLiteral("clip_index"));

		reg.registerTool(
			QStringLiteral("delete_clip"),
			QStringLiteral("Deletes a clip entirely from a track."),
			props,
			req,
			handleDeleteClip
		);
	}

	// 8. set_step_state
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the target track (default: in Beat+Bassline editor).");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propStep;
		propStep[QStringLiteral("type")] = QStringLiteral("integer");
		propStep[QStringLiteral("description")] = QStringLiteral("Step index (0-indexed, typically 0 to 15).");
		props[QStringLiteral("step_index")] = propStep;

		QJsonObject propActive;
		propActive[QStringLiteral("type")] = QStringLiteral("boolean");
		propActive[QStringLiteral("description")] = QStringLiteral("True to activate the step, false to deactivate.");
		props[QStringLiteral("active")] = propActive;

		QJsonObject propPattern;
		propPattern[QStringLiteral("type")] = QStringLiteral("integer");
		propPattern[QStringLiteral("description")] = QStringLiteral("Beat+Bassline pattern index (default: 0).");
		props[QStringLiteral("pattern_index")] = propPattern;

		QJsonObject propBB;
		propBB[QStringLiteral("type")] = QStringLiteral("boolean");
		propBB[QStringLiteral("description")] = QStringLiteral("True to target Beat+Bassline editor, false for Song editor (default: true).");
		props[QStringLiteral("in_bb_editor")] = propBB;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));
		req.append(QStringLiteral("step_index"));
		req.append(QStringLiteral("active"));

		reg.registerTool(
			QStringLiteral("set_step_state"),
			QStringLiteral("Sets the active state of an individual step for 16-step drum rhythms in LMMS's Beat+Bassline editor."),
			props,
			req,
			handleSetStepState
		);
	}

	// 9. control_bb_pattern
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the target track in Beat+Bassline editor.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propSteps;
		propSteps[QStringLiteral("description")] = QStringLiteral("Array of 16 step states (booleans or 0/1), or a 16-character pattern string (e.g. 'x...x...x...x...').");
		props[QStringLiteral("steps")] = propSteps;

		QJsonObject propPattern;
		propPattern[QStringLiteral("type")] = QStringLiteral("integer");
		propPattern[QStringLiteral("description")] = QStringLiteral("Pattern index (default: 0).");
		props[QStringLiteral("pattern_index")] = propPattern;

		QJsonObject propClear;
		propClear[QStringLiteral("type")] = QStringLiteral("boolean");
		propClear[QStringLiteral("description")] = QStringLiteral("If true, clears existing notes on the pattern clip before applying (default: false).");
		props[QStringLiteral("clear_first")] = propClear;

		QJsonObject propBB;
		propBB[QStringLiteral("type")] = QStringLiteral("boolean");
		propBB[QStringLiteral("description")] = QStringLiteral("Target Beat+Bassline editor (default: true).");
		props[QStringLiteral("in_bb_editor")] = propBB;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));
		req.append(QStringLiteral("steps"));

		reg.registerTool(
			QStringLiteral("control_bb_pattern"),
			QStringLiteral("Fast, direct programming of an entire 16-step drum rhythm in LMMS's Beat+Bassline editor."),
			props,
			req,
			handleControlBBPattern
		);
	}

	// 10. add_automation_node
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the target AutomationTrack.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propClip;
		propClip[QStringLiteral("type")] = QStringLiteral("integer");
		propClip[QStringLiteral("description")] = QStringLiteral("Index of the automation clip (default: 0). Creates clip if it does not exist.");
		props[QStringLiteral("clip_index")] = propClip;

		QJsonObject propStartBar;
		propStartBar[QStringLiteral("type")] = QStringLiteral("number");
		propStartBar[QStringLiteral("description")] = QStringLiteral("Start bar position if creating a new clip (default: 0).");
		props[QStringLiteral("start_bar")] = propStartBar;

		QJsonObject propBar;
		propBar[QStringLiteral("type")] = QStringLiteral("number");
		propBar[QStringLiteral("description")] = QStringLiteral("Node bar position (0-indexed relative to clip start).");
		props[QStringLiteral("bar")] = propBar;

		QJsonObject propTick;
		propTick[QStringLiteral("type")] = QStringLiteral("integer");
		propTick[QStringLiteral("description")] = QStringLiteral("Node tick offset within bar (default: 0).");
		props[QStringLiteral("tick")] = propTick;

		QJsonObject propVal;
		propVal[QStringLiteral("type")] = QStringLiteral("number");
		propVal[QStringLiteral("description")] = QStringLiteral("Normalized automation value (typically 0.0 to 1.0).");
		props[QStringLiteral("value")] = propVal;

		QJsonObject propNodes;
		propNodes[QStringLiteral("type")] = QStringLiteral("array");
		propNodes[QStringLiteral("description")] = QStringLiteral("Optional array of node objects [{'bar': 0.0, 'value': 0.0}, {'bar': 2.0, 'value': 1.0}] to add multiple nodes in batch.");
		props[QStringLiteral("nodes")] = propNodes;

		QJsonObject propPt;
		propPt[QStringLiteral("type")] = QStringLiteral("string");
		propPt[QStringLiteral("description")] = QStringLiteral("Progression curve type: 'linear', 'discrete', or 'cubic' (default: 'linear').");
		props[QStringLiteral("progression_type")] = propPt;

		QJsonObject propTension;
		propTension[QStringLiteral("type")] = QStringLiteral("number");
		propTension[QStringLiteral("description")] = QStringLiteral("Curve tension value for cubic Hermite interpolation.");
		props[QStringLiteral("tension")] = propTension;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));

		reg.registerTool(
			QStringLiteral("add_automation_node"),
			QStringLiteral("Adds automation curve nodes/points for filter sweeps, volume swells, reverb ducking, and tempo changes."),
			props,
			req,
			handleAddAutomationNode
		);
	}

	// 11. link_control
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the AutomationTrack holding the automation clip.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propClip;
		propClip[QStringLiteral("type")] = QStringLiteral("integer");
		propClip[QStringLiteral("description")] = QStringLiteral("Index of the automation clip on the track (default: 0).");
		props[QStringLiteral("clip_index")] = propClip;

		QJsonObject propTargetType;
		propTargetType[QStringLiteral("type")] = QStringLiteral("string");
		propTargetType[QStringLiteral("description")] = QStringLiteral("Target parameter type: 'tempo', 'master_volume', 'track_volume', 'track_panning', 'track_pitch', 'mixer_volume', or 'instrument_param'.");
		props[QStringLiteral("target_type")] = propTargetType;

		QJsonObject propTargetTrack;
		propTargetTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTargetTrack[QStringLiteral("description")] = QStringLiteral("Target track index for 'track_volume', 'track_panning', 'track_pitch', or 'instrument_param'.");
		props[QStringLiteral("target_track_index")] = propTargetTrack;

		QJsonObject propMixerCh;
		propMixerCh[QStringLiteral("type")] = QStringLiteral("integer");
		propMixerCh[QStringLiteral("description")] = QStringLiteral("Mixer channel index for 'mixer_volume'.");
		props[QStringLiteral("mixer_channel")] = propMixerCh;

		QJsonObject propParamName;
		propParamName[QStringLiteral("type")] = QStringLiteral("string");
		propParamName[QStringLiteral("description")] = QStringLiteral("Parameter name for 'instrument_param' (e.g. 'cutoff', 'resonance', 'attack', 'decay', 'osc1_wave').");
		props[QStringLiteral("param_name")] = propParamName;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));
		req.append(QStringLiteral("target_type"));

		reg.registerTool(
			QStringLiteral("link_control"),
			QStringLiteral("Links a song, track, instrument, or mixer parameter model to an automation clip."),
			props,
			req,
			handleLinkControl
		);
	}

	// 12. delete_track
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the track to delete (0-indexed).");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));

		reg.registerTool(
			QStringLiteral("delete_track"),
			QStringLiteral("Deletes an entire track and all its associated clips from the song timeline."),
			props,
			req,
			handleDeleteTrack
		);
	}

	// 13. clone_track
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the track to duplicate/clone.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));

		reg.registerTool(
			QStringLiteral("clone_track"),
			QStringLiteral("Duplicates an existing track including all its instrument settings, mixer routing, and timeline clips."),
			props,
			req,
			handleCloneTrack
		);
	}

	// 14. control_track
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the track to modify.");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propName;
		propName[QStringLiteral("type")] = QStringLiteral("string");
		propName[QStringLiteral("description")] = QStringLiteral("New track name.");
		props[QStringLiteral("name")] = propName;

		QJsonObject propVol;
		propVol[QStringLiteral("type")] = QStringLiteral("number");
		propVol[QStringLiteral("description")] = QStringLiteral("Track volume (0 to 200, default: 100).");
		props[QStringLiteral("volume")] = propVol;

		QJsonObject propPan;
		propPan[QStringLiteral("type")] = QStringLiteral("number");
		propPan[QStringLiteral("description")] = QStringLiteral("Track stereo panning (-100 to 100, default: 0).");
		props[QStringLiteral("panning")] = propPan;

		QJsonObject propPitch;
		propPitch[QStringLiteral("type")] = QStringLiteral("integer");
		propPitch[QStringLiteral("description")] = QStringLiteral("Track coarse pitch adjustment in semitones.");
		props[QStringLiteral("pitch")] = propPitch;

		QJsonObject propSolo;
		propSolo[QStringLiteral("type")] = QStringLiteral("boolean");
		propSolo[QStringLiteral("description")] = QStringLiteral("Set solo state on track.");
		props[QStringLiteral("solo")] = propSolo;

		QJsonObject propMuted;
		propMuted[QStringLiteral("type")] = QStringLiteral("boolean");
		propMuted[QStringLiteral("description")] = QStringLiteral("Set mute state on track.");
		props[QStringLiteral("muted")] = propMuted;

		QJsonObject propMixerCh;
		propMixerCh[QStringLiteral("type")] = QStringLiteral("integer");
		propMixerCh[QStringLiteral("description")] = QStringLiteral("Send output of this track to specified FX mixer channel index (0 = Master).");
		props[QStringLiteral("mixer_channel")] = propMixerCh;

		QJsonArray req;
		req.append(QStringLiteral("track_index"));

		reg.registerTool(
			QStringLiteral("control_track"),
			QStringLiteral("Renames, adjusts track-level volume, panning, pitch, solo/mute states, and mixer output channel directly."),
			props,
			req,
			handleControlTrack
		);
	}
}

} // namespace lmms
