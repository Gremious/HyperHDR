/* InfiniteSmoothing.cpp
*
*  MIT License
*
*  Copyright (c) 2020-2025 awawa-dev
*
*  Project homesite: https://github.com/awawa-dev/HyperHDR
*
*  Permission is hereby granted, free of charge, to any person obtaining a copy
*  of this software and associated documentation files (the "Software"), to deal
*  in the Software without restriction, including without limitation the rights
*  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
*  copies of the Software, and to permit persons to whom the Software is
*  furnished to do so, subject to the following conditions:
*
*  The above copyright notice and this permission notice shall be included in all
*  copies or substantial portions of the Software.

*  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
*  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
*  SOFTWARE.
 */

#ifndef PCH_ENABLED
	#include <QTimer>
	#include <QThread>	

	#include <algorithm>
	#include <chrono>
	#include <iomanip>
	#include <iostream>
	#include <limits>
	#include <tuple>
	#include <iomanip>
	#include <vector>
	#include <iostream>
	#include <iomanip>
	#include <tuple>
	#include <cassert>
	#include <cmath>
	#include <cstdint>
#endif

#include <limits>
#include <QMutexLocker>

#include <infinite-color-engine/InfiniteSmoothing.h>
#include <infinite-color-engine/InfiniteStepperInterpolator.h>
#include <infinite-color-engine/InfiniteYuvInterpolator.h>
#include <infinite-color-engine/InfiniteRgbInterpolator.h>
#include <infinite-color-engine/InfiniteHybridInterpolator.h>
#include <base/HyperHdrInstance.h>

using namespace hyperhdr;
using namespace linalg::aliases;

namespace
{
	constexpr auto SMOOTHING_USER_CONFIG = 0;
	constexpr int64_t  DEFAUL_SETTLINGTIME = 200;   // settlingtime in ms
	constexpr double   DEFAUL_UPDATEFREQUENCY = 25;    // updatefrequncy in hz
	constexpr double   MINIMAL_UPDATEFREQUENCY = 20;
}

InfiniteSmoothing::InfiniteSmoothing(const QJsonDocument& config, HyperHdrInstance* hyperhdr)
	: QObject(),
	_log(Logger::getInstance(QString("SMOOTHING%1").arg(hyperhdr->getInstanceIndex()))),
	_hyperhdr(hyperhdr),
	_continuousOutput(false),
	_flushFrame(false),
	_currentConfigId(SMOOTHING_USER_CONFIG),
	_enabled(false),
	_connected(false),
	_smoothingType(SmoothingType::Stepper),
	_interpolator(std::make_unique<InfiniteStepperInterpolator>()),
	_infoUpdate(true),
	_infoInput(true),
	_coolDown(0)
{
	// init cfg 0 (SMOOTHING_USER_CONFIG)
	addConfig(DEFAUL_SETTLINGTIME, DEFAUL_UPDATEFREQUENCY);
	handleSignalInstanceSettingsChanged(settings::type::SMOOTHING, config);
	selectConfig(SMOOTHING_USER_CONFIG);
}

void InfiniteSmoothing::clearQueuedColors(bool deviceEnabled, bool restarting)
{
	QMutexLocker locker(&_dataSynchro);

	try
	{
		Info(_log, "Clearing queued colors before: %s%s",
			(deviceEnabled) ? "enabling" : "disabling",
			(restarting) ? ". Smoothing configuration changed: restarting timer." : "");

		if ((!deviceEnabled || restarting) && _connected)
		{
			_connected = false;
			disconnect(this, &InfiniteSmoothing::SignalMasterClockTick, this, &InfiniteSmoothing::updateLeds);
		}

		if (_smoothingType == SmoothingType::HybridInterpolator)
		{
			_interpolator = std::make_unique<InfiniteHybridInterpolator>();
		}
		else if (_smoothingType == SmoothingType::YuvInterpolator)
		{
			_interpolator = std::make_unique<InfiniteYuvInterpolator>();
		}
		else if (_smoothingType == SmoothingType::RgbInterpolator)
		{
			_interpolator = std::make_unique<InfiniteRgbInterpolator>();
		}
		else
		{
			_interpolator = std::make_unique<InfiniteStepperInterpolator>();
		}
		
		_flushFrame = false;
		_infoUpdate = true;
		_infoInput = true;
		_coolDown = 0;

		if (deviceEnabled && !_connected)
		{
			_connected = true;
			connect(this, &InfiniteSmoothing::SignalMasterClockTick, this, &InfiniteSmoothing::updateLeds, Qt::DirectConnection);
		}

		emit _hyperhdr->SignalSmoothingRestarted(this->getSuggestedInterval());

		Info(_log, "Smoothing queue is cleared");
	}
	catch (...)
	{
		Debug(_log, "Smoothing error detected");
	}
}

void InfiniteSmoothing::handleSignalInstanceSettingsChanged(settings::type type, const QJsonDocument& config)
{
	if (type == settings::type::SMOOTHING)
	{
		if (InternalClock::isPreciseSteady())
			Info(_log, "High resolution clock is steady (good)");
		else
			Warning(_log, "High resolution clock is NOT STEADY!");

		QJsonObject obj = config.object();

		if (isEnabled() != obj["enable"].toBool(true))
		{
			setEnable(obj["enable"].toBool(true));
		}

		_continuousOutput = obj["continuousOutput"].toBool(true);

		_configurations[SMOOTHING_USER_CONFIG] = std::make_unique<SmoothingConfig>(
			SmoothingConfig{
			.pause = false,
			.settlingTime = static_cast<int64_t>(obj["time_ms"].toInt(DEFAUL_SETTLINGTIME)),
			.updateInterval = static_cast<int64_t>(std::round(std::max(1000.0 / std::max(obj["updateFrequency"].toDouble(DEFAUL_UPDATEFREQUENCY), MINIMAL_UPDATEFREQUENCY), 5.0))),
			.type = StringToEnumSmoothingType(obj["type"].toString()) }
		);

		auto& cfg = _configurations[SMOOTHING_USER_CONFIG];
		Info(_log, "Updating user config (%d) => type: %s, pause: %s, settlingTime: %ims, interval: %ims (%iHz)",
			SMOOTHING_USER_CONFIG, QSTRING_CSTR(EnumSmoothingTypeToString(cfg->type)), (cfg->pause) ? "true" : "false", int(cfg->settlingTime), int(cfg->updateInterval), int(1000.0 / cfg->updateInterval));

		if (_currentConfigId == SMOOTHING_USER_CONFIG)
		{
			if (isEnabled())
				QUEUE_CALL_0(_hyperhdr, update);
			selectConfig(SMOOTHING_USER_CONFIG);
		}
	}
}

void InfiniteSmoothing::incomingColors(std::vector<float3>&& linearRgbColors)
{
	if (_infoInput)
	{
		if (!isEnabled())
			Info(_log, "Smoothing is disabled. Direct output.");
		else 
			Info(_log, "Using %s smoothing input (%i)", QSTRING_CSTR(EnumSmoothingTypeToString(_smoothingType)), _currentConfigId);
		_infoInput = false;
	}


	if (!isEnabled())
	{
		queueColors(std::make_shared<std::vector<float3>>(std::move(linearRgbColors)));
		return;
	}

	_coolDown = 1;

	_dataSynchro.lock();
	_interpolator->setTargetColors(std::move(linearRgbColors), InternalClock::now());
	_dataSynchro.unlock();

	return;
}

void InfiniteSmoothing::updateLeds()
{	
	SharedOutputColors linearRgbColors;

	{
		QMutexLocker locker(&_dataSynchro);
		if (!isEnabled())
			return;
		_interpolator->updateCurrentColors(InternalClock::now());
		linearRgbColors = _interpolator->getCurrentColors();
	}

	queueColors(std::move(linearRgbColors));
}

void InfiniteSmoothing::queueColors(SharedOutputColors&& linearRgbColors)
{
	emit SignalProcessedColors(linearRgbColors);
}

void InfiniteSmoothing::handleSignalRequestComponent(hyperhdr::Components component, bool state)
{
	_flushFrame = state;

	if (component == hyperhdr::COMP_LEDDEVICE)
	{
		clearQueuedColors(state);
	}

	if (component == hyperhdr::COMP_SMOOTHING)
	{
		setEnable(state);
	}
}

void InfiniteSmoothing::setEnable(bool enable)
{
	_enabled = enable;

	clearQueuedColors(isEnabled());

	_hyperhdr->setNewComponentState(hyperhdr::COMP_SMOOTHING, enable);
}

unsigned InfiniteSmoothing::addConfig(int settlingTime_ms, double ledUpdateFrequency_hz, bool pause)
{
	_configurations.push_back(std::make_unique<SmoothingConfig>(
		SmoothingConfig{ .pause = pause, .settlingTime = settlingTime_ms, .updateInterval = int64_t(1000.0 / ledUpdateFrequency_hz) }));

	return static_cast<unsigned>(_configurations.size() - 1);
}

unsigned InfiniteSmoothing::addCustomSmoothingConfig(unsigned cfgID, int settlingTime_ms, double ledUpdateFrequency_hz, bool pause)
{
	int64_t interval =  (ledUpdateFrequency_hz > std::numeric_limits<double>::epsilon()) ? static_cast<int64_t>(1000.0 / ledUpdateFrequency_hz) : 10;
	
	if (cfgID < static_cast<unsigned>(_configurations.size()))
	{
		_configurations[cfgID] = std::make_unique<SmoothingConfig>(SmoothingConfig{ .pause = pause, .settlingTime = settlingTime_ms, .updateInterval = interval, .type = SmoothingType::Stepper });
		return cfgID;
	}
	else
	{
		for (unsigned int currentCfgID = SMOOTHING_EFFECT_CONFIGS_START; currentCfgID < _configurations.size(); currentCfgID++)
		{
			auto& element = _configurations[currentCfgID];
			if ((element->settlingTime == settlingTime_ms &&
				element->updateInterval == interval) ||
				(pause && element->pause == pause))
			{
				return currentCfgID;
			}
		}
		return addConfig(settlingTime_ms, ledUpdateFrequency_hz, pause);
	}
}

void InfiniteSmoothing::setCurrentSmoothingConfigParams(unsigned cfgID)
{
	if (cfgID != _currentConfigId)
	{
		selectConfig(cfgID);
	}
}

int InfiniteSmoothing::getSuggestedInterval()
{
	return (isEnabled()) ? static_cast<int>(_configurations[_currentConfigId]->updateInterval) : 0;
}

bool InfiniteSmoothing::selectConfig(unsigned cfgId)
{
	bool result = (cfgId < (unsigned)_configurations.size());

	_currentConfigId = (result) ? cfgId : SMOOTHING_USER_CONFIG;

	clearQueuedColors(isEnabled(), true);		

	Info(_log, "Selecting config (%d) => type: %s, pause: %s, settlingTime: %ims, interval: %ims (%iHz). Smoothing is currently: %s",
		_currentConfigId, QSTRING_CSTR(EnumSmoothingTypeToString(_smoothingType)), (!_configurations[_currentConfigId]->pause) ? "true" : "false",
		int(_configurations[_currentConfigId]->settlingTime),
		int(_configurations[_currentConfigId]->updateInterval),
		int(1000.0 / _configurations[_currentConfigId]->updateInterval),
		(_enabled) ? "enabled" : "disabled");

	return result;
}

bool InfiniteSmoothing::isEnabled() const
{
	return _enabled && !_configurations[_currentConfigId]->pause;
}

QString InfiniteSmoothing::EnumSmoothingTypeToString(SmoothingType type)
{
	if (type == SmoothingType::RgbInterpolator)
		return QString("RgbInterpolator");
	else if (type == SmoothingType::YuvInterpolator)
		return QString("YuvInterpolator");
	else if (type == SmoothingType::HybridInterpolator)
		return QString("HybridInterpolator");

	return QString("Stepper");
}

InfiniteSmoothing::SmoothingType InfiniteSmoothing::StringToEnumSmoothingType(QString name)
{
	if (name == QString("RgbInterpolator"))
		return SmoothingType::RgbInterpolator;
	else if (name == QString("YuvInterpolator"))
		return SmoothingType::YuvInterpolator;
	else if (name == QString("HybridInterpolator"))
		return SmoothingType::HybridInterpolator;

	return SmoothingType::Stepper;
}




/*
* _currentColors = _targetColorsCopy.deepClone();
		_previousTime = now;

		if (_flushFrame)
			queueColors(_currentColors.toVectorColorRgb());

		if (!_continuousOutput && _coolDown > 0)
		{
			_coolDown--;
			_flushFrame = true;
		}
		else
			_flushFrame = _continuousOutput;*/


