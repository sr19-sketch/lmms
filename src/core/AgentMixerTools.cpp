/*
 * AgentMixerTools.cpp - FX Mixer and Effect tools for AI Agent in LMMS
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

#include "AgentMixerTools.h"
#include "AgentToolRegistry.h"

#include "Engine.h"
#include "Song.h"
#include "Track.h"
#include "InstrumentTrack.h"
#include "SampleTrack.h"
#include "Mixer.h"
#include "EffectChain.h"
#include "Effect.h"
#include "EffectControls.h"
#include "PluginFactory.h"
#include "Plugin.h"
#include "GuiApplication.h"
#include "MixerView.h"

#include <QSet>
#include <algorithm>
#include <cmath>

namespace lmms
{

extern PluginFactory* getPluginFactory();

static void syncMixerGUI()
{
	if (gui::getGUI() && gui::getGUI()->mixerView())
	{
		gui::getGUI()->mixerView()->refreshDisplay();
	}
}

static void applyEffectControls(Effect* eff, const QJsonObject& controlsObj)
{
	if (!eff || controlsObj.isEmpty()) return;

	QList<AutomatableModel*> models;
	if (eff->controls())
	{
		models.append(eff->controls()->findChildren<AutomatableModel*>());
	}
	models.append(eff->findChildren<AutomatableModel*>());

	for (auto it = controlsObj.constBegin(); it != controlsObj.constEnd(); ++it)
	{
		QString key = it.key();
		float val = static_cast<float>(it.value().toDouble());
		QString simplifiedKey = key.toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' '));

		for (AutomatableModel* model : models)
		{
			if (!model) continue;
			QString mName = model->displayName().toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' '));
			QString mObjName = model->objectName().toLower().remove(QLatin1Char('_')).remove(QLatin1Char(' '));

			if (mName == simplifiedKey || mObjName == simplifiedKey ||
			    (!mName.isEmpty() && mName.contains(simplifiedKey)) ||
			    (!mObjName.isEmpty() && mObjName.contains(simplifiedKey)))
			{
				model->setValue(val);
				break;
			}
		}
	}
}

static QJsonArray getEffectControlsInfo(Effect* eff)
{
	QJsonArray result;
	if (!eff) return result;

	QList<AutomatableModel*> models;
	if (eff->controls())
	{
		models.append(eff->controls()->findChildren<AutomatableModel*>());
	}
	models.append(eff->findChildren<AutomatableModel*>());

	QSet<AutomatableModel*> seen;
	for (AutomatableModel* model : models)
	{
		if (!model || seen.contains(model)) continue;
		seen.insert(model);

		QString name = model->displayName();
		if (name.isEmpty()) name = model->objectName();
		if (name.isEmpty()) continue;

		QJsonObject p;
		p[QStringLiteral("name")] = name;
		p[QStringLiteral("value")] = model->value<float>();
		p[QStringLiteral("min")] = model->minValue<float>();
		p[QStringLiteral("max")] = model->maxValue<float>();
		result.append(p);
	}
	return result;
}

static QJsonObject handleCreateMixerChannel(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Mixer* mixer = Engine::mixer();
	Song* song = Engine::getSong();
	if (!mixer || !song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Mixer or Song engine is not available.");
		return QJsonObject();
	}

	int chIdx = mixer->createChannel();
	MixerChannel* ch = mixer->mixerChannel(chIdx);
	if (!ch)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to retrieve newly created mixer channel.");
		return QJsonObject();
	}

	if (args.contains(QStringLiteral("name")))
	{
		QString name = args.value(QStringLiteral("name")).toString().trimmed();
		if (!name.isEmpty())
		{
			ch->m_name = name;
			ch->m_volumeModel.setDisplayName(ch->m_name + QStringLiteral(">Volume"));
			ch->m_muteModel.setDisplayName(ch->m_name + QStringLiteral(">Mute"));
			ch->m_soloModel.setDisplayName(ch->m_name + QStringLiteral(">Solo"));
		}
	}

	if (args.contains(QStringLiteral("volume")))
	{
		float vol = static_cast<float>(args.value(QStringLiteral("volume")).toDouble(1.0));
		ch->m_volumeModel.setValue(std::clamp(vol, 0.0f, 2.0f));
	}

	bool routeToMaster = args.value(QStringLiteral("route_to_master")).toBool(true);
	if (!routeToMaster)
	{
		mixer->deleteChannelSend(chIdx, 0);
	}

	int maxCh = static_cast<int>(mixer->numChannels()) - 1;
	for (Track* trk : song->tracks())
	{
		if (auto it = dynamic_cast<InstrumentTrack*>(trk))
		{
			it->mixerChannelModel()->setRange(0, maxCh);
		}
		else if (auto st = dynamic_cast<SampleTrack*>(trk))
		{
			st->mixerChannelModel()->setRange(0, maxCh);
		}
	}

	song->setModified();
	syncMixerGUI();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Mixer channel created successfully.");
	data[QStringLiteral("channel_index")] = chIdx;
	data[QStringLiteral("name")] = ch->m_name;
	data[QStringLiteral("volume")] = ch->m_volumeModel.value();
	data[QStringLiteral("routed_to_master")] = (mixer->channelSendModel(chIdx, 0) != nullptr);
	return data;
}

static QJsonObject handleSetTrackMixerChannel(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Mixer* mixer = Engine::mixer();
	Song* song = Engine::getSong();
	if (!mixer || !song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Mixer or Song engine is not available.");
		return QJsonObject();
	}

	if (!args.contains(QStringLiteral("track_index")) || !args.contains(QStringLiteral("channel_index")))
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameters: 'track_index' and 'channel_index'.");
		return QJsonObject();
	}

	int trackIdx = args.value(QStringLiteral("track_index")).toInt(-1);
	int channelIdx = args.value(QStringLiteral("channel_index")).toInt(-1);

	const auto& tracks = song->tracks();
	if (trackIdx < 0 || trackIdx >= static_cast<int>(tracks.size()))
	{
		ok = false;
		errorMsg = QStringLiteral("Invalid track_index: %1. Total tracks: %2.").arg(trackIdx).arg(tracks.size());
		return QJsonObject();
	}

	if (channelIdx < 0 || channelIdx >= static_cast<int>(mixer->numChannels()))
	{
		ok = false;
		errorMsg = QStringLiteral("Invalid channel_index: %1. Total mixer channels: %2.").arg(channelIdx).arg(mixer->numChannels());
		return QJsonObject();
	}

	Track* t = tracks[trackIdx];
	if (auto it = dynamic_cast<InstrumentTrack*>(t))
	{
		it->mixerChannelModel()->setRange(0, static_cast<int>(mixer->numChannels()) - 1);
		it->mixerChannelModel()->setValue(channelIdx);
	}
	else if (auto st = dynamic_cast<SampleTrack*>(t))
	{
		st->mixerChannelModel()->setRange(0, static_cast<int>(mixer->numChannels()) - 1);
		st->mixerChannelModel()->setValue(channelIdx);
	}
	else
	{
		ok = false;
		errorMsg = QStringLiteral("Track at index %1 ('%2') is of type '%3', which does not output audio directly to a mixer channel (only InstrumentTrack and SampleTrack are routable).")
			.arg(trackIdx).arg(t->name()).arg(t->displayName());
		return QJsonObject();
	}

	song->setModified();
	syncMixerGUI();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Track routed to mixer channel successfully.");
	data[QStringLiteral("track_index")] = trackIdx;
	data[QStringLiteral("track_name")] = t->name();
	data[QStringLiteral("channel_index")] = channelIdx;
	data[QStringLiteral("channel_name")] = mixer->mixerChannel(channelIdx)->m_name;
	return data;
}

static QJsonObject handleControlMixerChannel(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Mixer* mixer = Engine::mixer();
	Song* song = Engine::getSong();
	if (!mixer || !song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Mixer or Song engine is not available.");
		return QJsonObject();
	}

	if (!args.contains(QStringLiteral("channel_index")))
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameter: 'channel_index'.");
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

	if (args.contains(QStringLiteral("name")))
	{
		QString newName = args.value(QStringLiteral("name")).toString().trimmed();
		if (!newName.isEmpty())
		{
			ch->m_name = newName;
			ch->m_volumeModel.setDisplayName(ch->m_name + QStringLiteral(">Volume"));
			ch->m_muteModel.setDisplayName(ch->m_name + QStringLiteral(">Mute"));
			ch->m_soloModel.setDisplayName(ch->m_name + QStringLiteral(">Solo"));
		}
	}

	if (args.contains(QStringLiteral("volume")))
	{
		float vol = static_cast<float>(args.value(QStringLiteral("volume")).toDouble());
		ch->m_volumeModel.setValue(std::clamp(vol, 0.0f, 2.0f));
	}

	if (args.contains(QStringLiteral("mute")))
	{
		bool mute = args.value(QStringLiteral("mute")).toBool();
		ch->m_muteModel.setValue(mute);
	}

	if (args.contains(QStringLiteral("solo")))
	{
		bool solo = args.value(QStringLiteral("solo")).toBool();
		ch->m_soloModel.setValue(solo);
		mixer->toggledSolo();
	}

	if (args.contains(QStringLiteral("send_to_channel")))
	{
		int sendTo = args.value(QStringLiteral("send_to_channel")).toInt(-1);
		if (sendTo < 0 || sendTo >= static_cast<int>(mixer->numChannels()))
		{
			ok = false;
			errorMsg = QStringLiteral("Invalid send_to_channel: %1. Total mixer channels: %2.").arg(sendTo).arg(mixer->numChannels());
			return QJsonObject();
		}
		if (sendTo == channelIdx)
		{
			ok = false;
			errorMsg = QStringLiteral("Cannot send mixer channel %1 to itself.").arg(channelIdx);
			return QJsonObject();
		}

		bool deleteSend = args.value(QStringLiteral("delete_send")).toBool(false);
		float sendAmt = static_cast<float>(args.value(QStringLiteral("send_amount")).toDouble(1.0));
		if (deleteSend || sendAmt <= 0.0f)
		{
			mixer->deleteChannelSend(channelIdx, sendTo);
		}
		else
		{
			mixer->createChannelSend(channelIdx, sendTo, std::clamp(sendAmt, 0.0f, 2.0f));
		}
	}

	song->setModified();
	syncMixerGUI();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Mixer channel adjusted successfully.");
	data[QStringLiteral("channel_index")] = channelIdx;
	data[QStringLiteral("name")] = ch->m_name;
	data[QStringLiteral("volume")] = ch->m_volumeModel.value();
	data[QStringLiteral("mute")] = ch->m_muteModel.value();
	data[QStringLiteral("solo")] = ch->m_soloModel.value();
	return data;
}

static QJsonObject handleListAvailableEffects(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Q_UNUSED(args);

	PluginFactory* factory = PluginFactory::instance();
	if (!factory) factory = getPluginFactory();
	if (!factory)
	{
		ok = false;
		errorMsg = QStringLiteral("PluginFactory is not available.");
		return QJsonObject();
	}

	Plugin::DescriptorList effDescs = factory->descriptors(Plugin::Type::Effect);

	QJsonArray effects;
	for (const Plugin::Descriptor* desc : effDescs)
	{
		if (!desc) continue;

		QJsonObject effObj;
		effObj[QStringLiteral("name")] = QString::fromUtf8(desc->name);
		effObj[QStringLiteral("display_name")] = QString::fromUtf8(desc->displayName ? desc->displayName : desc->name);
		effObj[QStringLiteral("description")] = QString::fromUtf8(desc->description ? desc->description : "");
		effObj[QStringLiteral("author")] = QString::fromUtf8(desc->author ? desc->author : "");
		effObj[QStringLiteral("version")] = desc->version;
		effects.append(effObj);
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("count")] = effects.size();
	data[QStringLiteral("effects")] = effects;
	return data;
}

static QJsonObject handleAddMixerEffect(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Mixer* mixer = Engine::mixer();
	Song* song = Engine::getSong();
	if (!mixer || !song)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Mixer or Song engine is not available.");
		return QJsonObject();
	}

	if (!args.contains(QStringLiteral("channel_index")) || !args.contains(QStringLiteral("effect_name")))
	{
		ok = false;
		errorMsg = QStringLiteral("Missing required parameters: 'channel_index' and 'effect_name'.");
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

	QString effectName = args.value(QStringLiteral("effect_name")).toString().trimmed();
	if (effectName.isEmpty())
	{
		ok = false;
		errorMsg = QStringLiteral("Parameter 'effect_name' cannot be empty.");
		return QJsonObject();
	}

	PluginFactory* factory = PluginFactory::instance();
	if (!factory) factory = getPluginFactory();
	QString canonicalName = effectName;
	if (factory)
	{
		Plugin::DescriptorList descs = factory->descriptors(Plugin::Type::Effect);
		for (const Plugin::Descriptor* desc : descs)
		{
			if (desc && (QString::compare(QString::fromUtf8(desc->name), effectName, Qt::CaseInsensitive) == 0 ||
			             (desc->displayName && QString::compare(QString::fromUtf8(desc->displayName), effectName, Qt::CaseInsensitive) == 0)))
			{
				canonicalName = QString::fromUtf8(desc->name);
				break;
			}
		}
	}

	Effect* eff = Effect::instantiate(canonicalName, &ch->m_fxChain, nullptr);
	if (!eff || !eff->isOkay())
	{
		delete eff;
		ok = false;
		errorMsg = QStringLiteral("Failed to instantiate effect '%1'.").arg(effectName);
		return QJsonObject();
	}

	if (args.contains(QStringLiteral("wet_dry")))
	{
		float wd = static_cast<float>(args.value(QStringLiteral("wet_dry")).toDouble(1.0));
		eff->wetDryModel()->setValue(std::clamp(wd, 0.0f, 1.0f));
	}

	if (args.contains(QStringLiteral("enabled")))
	{
		bool en = args.value(QStringLiteral("enabled")).toBool(true);
		eff->enabledModel()->setValue(en);
	}

	if (args.contains(QStringLiteral("controls")) && args.value(QStringLiteral("controls")).isObject())
	{
		applyEffectControls(eff, args.value(QStringLiteral("controls")).toObject());
	}

	ch->m_fxChain.appendEffect(eff);
	int effIdx = ch->m_fxChain.numOfEffects() - 1;

	song->setModified();
	syncMixerGUI();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Effect inserted successfully onto mixer channel.");
	data[QStringLiteral("channel_index")] = channelIdx;
	data[QStringLiteral("channel_name")] = ch->m_name;
	data[QStringLiteral("effect_index")] = effIdx;
	data[QStringLiteral("effect_name")] = canonicalName;
	data[QStringLiteral("display_name")] = QString::fromUtf8(eff->descriptor() && eff->descriptor()->displayName ? eff->descriptor()->displayName : (eff->descriptor() ? eff->descriptor()->name : ""));
	data[QStringLiteral("wet_dry")] = eff->wetLevel();
	data[QStringLiteral("enabled")] = eff->isEnabled();
	data[QStringLiteral("controls")] = getEffectControlsInfo(eff);
	return data;
}

static QJsonObject handleControlMixerEffect(const QJsonObject& args, bool& ok, QString& errorMsg)
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

	int channelIdx = args.value(QStringLiteral("channel_index")).toInt(-1);
	int effectIdx = args.value(QStringLiteral("effect_index")).toInt(-1);

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

	if (effectIdx < 0 || effectIdx >= ch->m_fxChain.numOfEffects())
	{
		ok = false;
		errorMsg = QStringLiteral("Invalid effect_index: %1. Channel %2 has %3 effect(s).").arg(effectIdx).arg(channelIdx).arg(ch->m_fxChain.numOfEffects());
		return QJsonObject();
	}

	Effect* eff = ch->m_fxChain.effect(effectIdx);
	if (!eff)
	{
		ok = false;
		errorMsg = QStringLiteral("Failed to retrieve effect at index %1 on channel %2.").arg(effectIdx).arg(channelIdx);
		return QJsonObject();
	}

	bool remove = args.value(QStringLiteral("remove")).toBool(false);
	if (remove)
	{
		ch->m_fxChain.removeEffect(eff);
		delete eff;
		song->setModified();

		ok = true;
		QJsonObject data;
		data[QStringLiteral("message")] = QStringLiteral("Effect removed successfully from mixer channel.");
		data[QStringLiteral("channel_index")] = channelIdx;
		data[QStringLiteral("removed_effect_index")] = effectIdx;
		data[QStringLiteral("remaining_effects")] = ch->m_fxChain.numOfEffects();
		syncMixerGUI();
		return data;
	}

	if (args.contains(QStringLiteral("wet_dry")))
	{
		float wd = static_cast<float>(args.value(QStringLiteral("wet_dry")).toDouble());
		eff->wetDryModel()->setValue(std::clamp(wd, 0.0f, 1.0f));
	}

	if (args.contains(QStringLiteral("enabled")))
	{
		bool en = args.value(QStringLiteral("enabled")).toBool();
		eff->enabledModel()->setValue(en);
	}

	if (args.contains(QStringLiteral("controls")) && args.value(QStringLiteral("controls")).isObject())
	{
		applyEffectControls(eff, args.value(QStringLiteral("controls")).toObject());
	}

	song->setModified();
	syncMixerGUI();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Mixer effect adjusted successfully.");
	data[QStringLiteral("channel_index")] = channelIdx;
	data[QStringLiteral("effect_index")] = effectIdx;
	data[QStringLiteral("effect_name")] = QString::fromUtf8(eff->descriptor() ? eff->descriptor()->name : "");
	data[QStringLiteral("wet_dry")] = eff->wetLevel();
	data[QStringLiteral("enabled")] = eff->isEnabled();
	data[QStringLiteral("controls")] = getEffectControlsInfo(eff);
	return data;
}

static QJsonObject handleGetMixerInfo(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Q_UNUSED(args);

	Mixer* mixer = Engine::mixer();
	if (!mixer)
	{
		ok = false;
		errorMsg = QStringLiteral("LMMS Mixer engine is not available.");
		return QJsonObject();
	}

	QJsonArray channelsArray;
	int totalChannels = static_cast<int>(mixer->numChannels());

	for (int i = 0; i < totalChannels; ++i)
	{
		MixerChannel* ch = mixer->mixerChannel(i);
		if (!ch) continue;

		QJsonObject chObj;
		chObj[QStringLiteral("channel_index")] = i;
		chObj[QStringLiteral("name")] = ch->m_name;
		chObj[QStringLiteral("is_master")] = ch->isMaster();
		chObj[QStringLiteral("volume")] = ch->m_volumeModel.value();
		chObj[QStringLiteral("mute")] = ch->m_muteModel.value();
		chObj[QStringLiteral("solo")] = ch->m_soloModel.value();

		// Sends
		QJsonArray sendsArray;
		for (MixerRoute* route : ch->m_sends)
		{
			if (!route || !route->receiver()) continue;
			QJsonObject sendObj;
			sendObj[QStringLiteral("to_channel")] = static_cast<int>(route->receiverIndex());
			sendObj[QStringLiteral("to_name")] = route->receiver()->m_name;
			sendObj[QStringLiteral("amount")] = route->amount()->value();
			sendsArray.append(sendObj);
		}
		chObj[QStringLiteral("sends")] = sendsArray;

		// Receives
		QJsonArray receivesArray;
		for (MixerRoute* route : ch->m_receives)
		{
			if (!route || !route->sender()) continue;
			QJsonObject recObj;
			recObj[QStringLiteral("from_channel")] = static_cast<int>(route->senderIndex());
			recObj[QStringLiteral("from_name")] = route->sender()->m_name;
			recObj[QStringLiteral("amount")] = route->amount()->value();
			receivesArray.append(recObj);
		}
		chObj[QStringLiteral("receives")] = receivesArray;

		// Effects
		QJsonArray fxArray;
		int fxCount = ch->m_fxChain.numOfEffects();
		for (int j = 0; j < fxCount; ++j)
		{
			Effect* eff = ch->m_fxChain.effect(j);
			if (!eff) continue;

			QJsonObject effObj;
			effObj[QStringLiteral("effect_index")] = j;
			effObj[QStringLiteral("name")] = QString::fromUtf8(eff->descriptor() ? eff->descriptor()->name : "");
			effObj[QStringLiteral("display_name")] = QString::fromUtf8(eff->descriptor() && eff->descriptor()->displayName ? eff->descriptor()->displayName : (eff->descriptor() ? eff->descriptor()->name : ""));
			effObj[QStringLiteral("enabled")] = eff->isEnabled();
			effObj[QStringLiteral("wet_dry")] = eff->wetLevel();
			effObj[QStringLiteral("controls")] = getEffectControlsInfo(eff);
			fxArray.append(effObj);
		}
		chObj[QStringLiteral("effects")] = fxArray;

		channelsArray.append(chObj);
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("num_channels")] = totalChannels;
	data[QStringLiteral("channels")] = channelsArray;
	return data;
}

static QJsonObject handleGetMixerPeakLevels(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Q_UNUSED(args);
	Mixer* mixer = Engine::mixer();
	if (!mixer)
	{
		ok = false;
		errorMsg = QStringLiteral("Mixer engine is not available.");
		return QJsonObject();
	}

	int numChannels = mixer->numChannels();
	QJsonArray channelsArr;
	bool masterClipping = false;

	for (int i = 0; i < numChannels; ++i)
	{
		MixerChannel* ch = mixer->mixerChannel(i);
		if (!ch) continue;

		float leftPeak = ch->m_peakLeft;
		float rightPeak = ch->m_peakRight;
		float maxPeak = std::max(leftPeak, rightPeak);
		bool clipping = (maxPeak >= 1.0f);

		if (i == 0 && clipping)
		{
			masterClipping = true;
		}

		float peakDb = maxPeak > 0.00001f ? 20.0f * std::log10(maxPeak) : -100.0f;
		float leftDb = leftPeak > 0.00001f ? 20.0f * std::log10(leftPeak) : -100.0f;
		float rightDb = rightPeak > 0.00001f ? 20.0f * std::log10(rightPeak) : -100.0f;

		QJsonObject chObj;
		chObj[QStringLiteral("channel_index")] = i;
		chObj[QStringLiteral("channel_name")] = (i == 0) ? QStringLiteral("Master") : (ch->m_name.isEmpty() ? QStringLiteral("FX %1").arg(i) : ch->m_name);
		chObj[QStringLiteral("peak_linear")] = maxPeak;
		chObj[QStringLiteral("peak_db")] = peakDb;
		chObj[QStringLiteral("peak_left_linear")] = leftPeak;
		chObj[QStringLiteral("peak_left_db")] = leftDb;
		chObj[QStringLiteral("peak_right_linear")] = rightPeak;
		chObj[QStringLiteral("peak_right_db")] = rightDb;
		chObj[QStringLiteral("clipping")] = clipping;
		chObj[QStringLiteral("muted")] = ch->m_muted;
		channelsArr.append(chObj);
	}

	ok = true;
	QJsonObject data;
	data[QStringLiteral("channel_count")] = numChannels;
	data[QStringLiteral("master_clipping")] = masterClipping;
	data[QStringLiteral("channels")] = channelsArr;
	return data;
}

static QJsonObject handleReorderMixerEffects(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Mixer* mixer = Engine::mixer();
	if (!mixer)
	{
		ok = false;
		errorMsg = QStringLiteral("Mixer engine is not available.");
		return QJsonObject();
	}

	int channelIndex = args.value(QStringLiteral("channel_index")).toInt(-1);
	int fromIndex = args.value(QStringLiteral("from_index")).toInt(-1);
	int toIndex = args.value(QStringLiteral("to_index")).toInt(-1);

	if (channelIndex < 0 || channelIndex >= mixer->numChannels())
	{
		ok = false;
		errorMsg = QStringLiteral("Channel index %1 is out of range.").arg(channelIndex);
		return QJsonObject();
	}

	MixerChannel* ch = mixer->mixerChannel(channelIndex);
	int numEffects = ch->m_fxChain.numOfEffects();

	if (fromIndex < 0 || fromIndex >= numEffects)
	{
		ok = false;
		errorMsg = QStringLiteral("from_index %1 is out of range (channel has %2 effects).").arg(fromIndex).arg(numEffects);
		return QJsonObject();
	}

	if (toIndex < 0 || toIndex >= numEffects)
	{
		ok = false;
		errorMsg = QStringLiteral("to_index %1 is out of range (channel has %2 effects).").arg(toIndex).arg(numEffects);
		return QJsonObject();
	}

	Effect* targetEffect = ch->m_fxChain.effect(fromIndex);
	if (!targetEffect)
	{
		ok = false;
		errorMsg = QStringLiteral("No effect found at index %1.").arg(fromIndex);
		return QJsonObject();
	}

	int current = fromIndex;
	while (current < toIndex)
	{
		ch->m_fxChain.moveDown(targetEffect);
		current++;
	}
	while (current > toIndex)
	{
		ch->m_fxChain.moveUp(targetEffect);
		current--;
	}

	syncMixerGUI();
	if (Engine::getSong()) Engine::getSong()->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Moved effect '%1' from position %2 to %3 in channel %4.")
		.arg(targetEffect->displayName()).arg(fromIndex).arg(toIndex).arg(channelIndex);
	data[QStringLiteral("channel_index")] = channelIndex;
	data[QStringLiteral("from_index")] = fromIndex;
	data[QStringLiteral("to_index")] = toIndex;
	data[QStringLiteral("effect_name")] = targetEffect->displayName();
	return data;
}

static QJsonObject handleDeleteMixerChannel(const QJsonObject& args, bool& ok, QString& errorMsg)
{
	Mixer* mixer = Engine::mixer();
	if (!mixer)
	{
		ok = false;
		errorMsg = QStringLiteral("Mixer engine is not available.");
		return QJsonObject();
	}

	int channelIndex = args.value(QStringLiteral("channel_index")).toInt(-1);
	if (channelIndex <= 0 || channelIndex >= mixer->numChannels())
	{
		ok = false;
		errorMsg = QStringLiteral("Channel index %1 is invalid (cannot delete Master channel 0, must be between 1 and %2).")
			.arg(channelIndex).arg(mixer->numChannels() - 1);
		return QJsonObject();
	}

	MixerChannel* ch = mixer->mixerChannel(channelIndex);
	QString chName = ch ? ch->m_name : QString::number(channelIndex);

	mixer->deleteChannel(channelIndex);

	syncMixerGUI();
	if (Engine::getSong()) Engine::getSong()->setModified();

	ok = true;
	QJsonObject data;
	data[QStringLiteral("message")] = QStringLiteral("Mixer channel %1 ('%2') deleted successfully. Connected tracks re-routed to Master.")
		.arg(channelIndex).arg(chName);
	data[QStringLiteral("deleted_channel_index")] = channelIndex;
	data[QStringLiteral("remaining_channels")] = mixer->numChannels();
	return data;
}

void registerMixerTools(AgentToolRegistry& reg)
{
	// 1. create_mixer_channel
	{
		QJsonObject props;
		QJsonObject propName;
		propName[QStringLiteral("type")] = QStringLiteral("string");
		propName[QStringLiteral("description")] = QStringLiteral("Optional custom name for the channel (e.g. 'Drum Bus', 'Lead Reverb', 'Bass Sub').");
		props[QStringLiteral("name")] = propName;

		QJsonObject propVol;
		propVol[QStringLiteral("type")] = QStringLiteral("number");
		propVol[QStringLiteral("description")] = QStringLiteral("Initial volume level (0.0 to 2.0, default 1.0).");
		props[QStringLiteral("volume")] = propVol;

		QJsonObject propRoute;
		propRoute[QStringLiteral("type")] = QStringLiteral("boolean");
		propRoute[QStringLiteral("description")] = QStringLiteral("Automatically route output to Master channel 0 (default: true).");
		props[QStringLiteral("route_to_master")] = propRoute;

		reg.registerTool(
			QStringLiteral("create_mixer_channel"),
			QStringLiteral("Creates a new mixer channel (bus) in the LMMS FX Mixer using direct engine calls. Connects master send by default."),
			props,
			QJsonArray(),
			handleCreateMixerChannel
		);
	}

	// 2. set_track_mixer_channel
	{
		QJsonObject props;
		QJsonObject propTrack;
		propTrack[QStringLiteral("type")] = QStringLiteral("integer");
		propTrack[QStringLiteral("description")] = QStringLiteral("Index of the track in the song editor (0-indexed).");
		props[QStringLiteral("track_index")] = propTrack;

		QJsonObject propChan;
		propChan[QStringLiteral("type")] = QStringLiteral("integer");
		propChan[QStringLiteral("description")] = QStringLiteral("Target mixer channel index (0 for Master, 1, 2, ...).");
		props[QStringLiteral("channel_index")] = propChan;

		QJsonArray required;
		required.append(QStringLiteral("track_index"));
		required.append(QStringLiteral("channel_index"));

		reg.registerTool(
			QStringLiteral("set_track_mixer_channel"),
			QStringLiteral("Assigns any instrument or sample track to a specific mixer channel directly in the engine."),
			props,
			required,
			handleSetTrackMixerChannel
		);
	}

	// 3. control_mixer_channel
	{
		QJsonObject props;
		QJsonObject propChan;
		propChan[QStringLiteral("type")] = QStringLiteral("integer");
		propChan[QStringLiteral("description")] = QStringLiteral("Channel index to adjust (0 is Master).");
		props[QStringLiteral("channel_index")] = propChan;

		QJsonObject propName;
		propName[QStringLiteral("type")] = QStringLiteral("string");
		propName[QStringLiteral("description")] = QStringLiteral("Rename the channel.");
		props[QStringLiteral("name")] = propName;

		QJsonObject propVol;
		propVol[QStringLiteral("type")] = QStringLiteral("number");
		propVol[QStringLiteral("description")] = QStringLiteral("Volume level (0.0 to 2.0).");
		props[QStringLiteral("volume")] = propVol;

		QJsonObject propMute;
		propMute[QStringLiteral("type")] = QStringLiteral("boolean");
		propMute[QStringLiteral("description")] = QStringLiteral("Mute state (true / false).");
		props[QStringLiteral("mute")] = propMute;

		QJsonObject propSolo;
		propSolo[QStringLiteral("type")] = QStringLiteral("boolean");
		propSolo[QStringLiteral("description")] = QStringLiteral("Solo state (true / false).");
		props[QStringLiteral("solo")] = propSolo;

		QJsonObject propSendTo;
		propSendTo[QStringLiteral("type")] = QStringLiteral("integer");
		propSendTo[QStringLiteral("description")] = QStringLiteral("Target mixer channel to send audio to (e.g. sending a synth to a Reverb bus).");
		props[QStringLiteral("send_to_channel")] = propSendTo;

		QJsonObject propSendAmt;
		propSendAmt[QStringLiteral("type")] = QStringLiteral("number");
		propSendAmt[QStringLiteral("description")] = QStringLiteral("Send level (0.0 to 2.0, default 1.0).");
		props[QStringLiteral("send_amount")] = propSendAmt;

		QJsonObject propDelSend;
		propDelSend[QStringLiteral("type")] = QStringLiteral("boolean");
		propDelSend[QStringLiteral("description")] = QStringLiteral("Set true to delete the send to send_to_channel.");
		props[QStringLiteral("delete_send")] = propDelSend;

		QJsonArray required;
		required.append(QStringLiteral("channel_index"));

		reg.registerTool(
			QStringLiteral("control_mixer_channel"),
			QStringLiteral("Controls volume, muting, soloing, renaming, and bus sends between mixer channels."),
			props,
			required,
			handleControlMixerChannel
		);
	}

	// 4. list_available_effects
	{
		reg.registerTool(
			QStringLiteral("list_available_effects"),
			QStringLiteral("Discovers all effect plugins available in LMMS (e.g. ReverbSC, Delay, Bitcrush, Compressor, Eq, DualFilter, etc.)."),
			QJsonObject(),
			QJsonArray(),
			handleListAvailableEffects
		);
	}

	// 5. add_mixer_effect
	{
		QJsonObject props;
		QJsonObject propChan;
		propChan[QStringLiteral("type")] = QStringLiteral("integer");
		propChan[QStringLiteral("description")] = QStringLiteral("Mixer channel to insert into (0 is Master).");
		props[QStringLiteral("channel_index")] = propChan;

		QJsonObject propEff;
		propEff[QStringLiteral("type")] = QStringLiteral("string");
		propEff[QStringLiteral("description")] = QStringLiteral("Plugin name (e.g. 'ReverbSC', 'Delay', 'Bitcrush', 'Compressor', 'Eq').");
		props[QStringLiteral("effect_name")] = propEff;

		QJsonObject propWd;
		propWd[QStringLiteral("type")] = QStringLiteral("number");
		propWd[QStringLiteral("description")] = QStringLiteral("Wet/dry balance from 0.0 (all dry) to 1.0 (all wet, default: 1.0).");
		props[QStringLiteral("wet_dry")] = propWd;

		QJsonObject propEn;
		propEn[QStringLiteral("type")] = QStringLiteral("boolean");
		propEn[QStringLiteral("description")] = QStringLiteral("Effect bypass state (default: true).");
		props[QStringLiteral("enabled")] = propEn;

		QJsonObject propCtrl;
		propCtrl[QStringLiteral("type")] = QStringLiteral("object");
		propCtrl[QStringLiteral("description")] = QStringLiteral("Optional key-value map of plugin parameters to initialize.");
		props[QStringLiteral("controls")] = propCtrl;

		QJsonArray required;
		required.append(QStringLiteral("channel_index"));
		required.append(QStringLiteral("effect_name"));

		reg.registerTool(
			QStringLiteral("add_mixer_effect"),
			QStringLiteral("Inserts an audio effect onto a mixer channel's FX chain directly in the engine."),
			props,
			required,
			handleAddMixerEffect
		);
	}

	// 6. control_mixer_effect
	{
		QJsonObject props;
		QJsonObject propChan;
		propChan[QStringLiteral("type")] = QStringLiteral("integer");
		propChan[QStringLiteral("description")] = QStringLiteral("Target mixer channel.");
		props[QStringLiteral("channel_index")] = propChan;

		QJsonObject propEffIdx;
		propEffIdx[QStringLiteral("type")] = QStringLiteral("integer");
		propEffIdx[QStringLiteral("description")] = QStringLiteral("Index of the effect in that channel's FX chain (0-indexed).");
		props[QStringLiteral("effect_index")] = propEffIdx;

		QJsonObject propWd;
		propWd[QStringLiteral("type")] = QStringLiteral("number");
		propWd[QStringLiteral("description")] = QStringLiteral("Adjust wet/dry mix (0.0 to 1.0).");
		props[QStringLiteral("wet_dry")] = propWd;

		QJsonObject propEn;
		propEn[QStringLiteral("type")] = QStringLiteral("boolean");
		propEn[QStringLiteral("description")] = QStringLiteral("Enable or bypass the effect.");
		props[QStringLiteral("enabled")] = propEn;

		QJsonObject propCtrl;
		propCtrl[QStringLiteral("type")] = QStringLiteral("object");
		propCtrl[QStringLiteral("description")] = QStringLiteral("Key-value map of plugin parameters (e.g. roomsize, input_gain, cutoff, resonance, drive).");
		props[QStringLiteral("controls")] = propCtrl;

		QJsonObject propRem;
		propRem[QStringLiteral("type")] = QStringLiteral("boolean");
		propRem[QStringLiteral("description")] = QStringLiteral("Set true to delete the effect from the chain.");
		props[QStringLiteral("remove")] = propRem;

		QJsonArray required;
		required.append(QStringLiteral("channel_index"));
		required.append(QStringLiteral("effect_index"));

		reg.registerTool(
			QStringLiteral("control_mixer_effect"),
			QStringLiteral("Modifies parameters, dry/wet mix, bypass state, or removes an effect on a mixer channel."),
			props,
			required,
			handleControlMixerEffect
		);
	}

	// 7. get_mixer_info
	{
		reg.registerTool(
			QStringLiteral("get_mixer_info"),
			QStringLiteral("Returns the complete state of the LMMS FX Mixer including all channels, volumes, mute/solo states, active effects in each FX rack, parameter controls, and send routes."),
			QJsonObject(),
			QJsonArray(),
			handleGetMixerInfo
		);
	}

	// 8. get_mixer_peak_levels
	{
		reg.registerTool(
			QStringLiteral("get_mixer_peak_levels"),
			QStringLiteral("Returns real-time peak linear and decibel levels (dBFS) for the Master channel and all numbered mixer channels to enable dynamic gain staging."),
			QJsonObject(),
			QJsonArray(),
			handleGetMixerPeakLevels
		);
	}

	// 9. reorder_mixer_effects
	{
		QJsonObject props;
		QJsonObject propChan;
		propChan[QStringLiteral("type")] = QStringLiteral("integer");
		propChan[QStringLiteral("description")] = QStringLiteral("Mixer channel index holding the effect rack.");
		props[QStringLiteral("channel_index")] = propChan;

		QJsonObject propFrom;
		propFrom[QStringLiteral("type")] = QStringLiteral("integer");
		propFrom[QStringLiteral("description")] = QStringLiteral("Current 0-indexed position of the effect plugin in the chain.");
		props[QStringLiteral("from_index")] = propFrom;

		QJsonObject propTo;
		propTo[QStringLiteral("type")] = QStringLiteral("integer");
		propTo[QStringLiteral("description")] = QStringLiteral("Target 0-indexed position of the effect plugin in the chain.");
		props[QStringLiteral("to_index")] = propTo;

		QJsonArray req;
		req.append(QStringLiteral("channel_index"));
		req.append(QStringLiteral("from_index"));
		req.append(QStringLiteral("to_index"));

		reg.registerTool(
			QStringLiteral("reorder_mixer_effects"),
			QStringLiteral("Moves an effect plugin up or down in a channel's FX chain (e.g. putting EQ before Compressor vs after Reverb)."),
			props,
			req,
			handleReorderMixerEffects
		);
	}

	// 10. delete_mixer_channel
	{
		QJsonObject props;
		QJsonObject propChan;
		propChan[QStringLiteral("type")] = QStringLiteral("integer");
		propChan[QStringLiteral("description")] = QStringLiteral("Index of the mixer channel to delete (must be > 0; Master channel 0 cannot be deleted).");
		props[QStringLiteral("channel_index")] = propChan;

		QJsonArray req;
		req.append(QStringLiteral("channel_index"));

		reg.registerTool(
			QStringLiteral("delete_mixer_channel"),
			QStringLiteral("Deletes an unused mixer channel and safely re-routes connected tracks back to Master."),
			props,
			req,
			handleDeleteMixerChannel
		);
	}
}

} // namespace lmms
