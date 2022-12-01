
// LedDevice includes
#include <leddevice/LedDevice.h>
#include "ProviderRs232.h"

// qt includes
#include <QSerialPortInfo>
#include <QEventLoop>
#include <QThread>

#include <chrono>
#include <utils/InternalClock.h>

// Constants
constexpr std::chrono::milliseconds WRITE_TIMEOUT{ 1000 };	// device write timeout in ms
constexpr std::chrono::milliseconds OPEN_TIMEOUT{ 5000 };		// device open timeout in ms
const int MAX_WRITE_TIMEOUTS = 5;	// Maximum number of allowed timeouts
const int NUM_POWEROFF_WRITE_BLACK = 3;	// Number of write "BLACK" during powering off

ProviderRs232::ProviderRs232(const QJsonObject& deviceConfig)
	: LedDevice(deviceConfig)
	, _rs232Port(this)
	, _baudRate_Hz(1000000)
	, _isAutoDeviceName(false)
	, _delayAfterConnect_ms(0)
	, _frameDropCounter(0)
	, _espHandshake(true)
{
}

bool ProviderRs232::init(const QJsonObject& deviceConfig)
{
	bool isInitOK = false;

	// Initialise sub-class
	if (LedDevice::init(deviceConfig))
	{

		Debug(_log, "DeviceType   : %s", QSTRING_CSTR(this->getActiveDeviceType()));
		Debug(_log, "LedCount     : %d", this->getLedCount());
		Debug(_log, "RefreshTime  : %d", _refreshTimerInterval_ms);

		_deviceName = deviceConfig["output"].toString("auto");

		// If device name was given as unix /dev/ system-location, get port name
		if (_deviceName.startsWith(QLatin1String("/dev/")))
			_deviceName = _deviceName.mid(5);

		_isAutoDeviceName = _deviceName.toLower() == "auto";
		_baudRate_Hz = deviceConfig["rate"].toInt();
		_delayAfterConnect_ms = deviceConfig["delayAfterConnect"].toInt(0);
		_espHandshake = deviceConfig["espHandshake"].toBool(false);

		Debug(_log, "Device name   : %s", QSTRING_CSTR(_deviceName));
		Debug(_log, "Auto selection: %d", _isAutoDeviceName);
		Debug(_log, "Baud rate     : %d", _baudRate_Hz);
		Debug(_log, "ESP handshake : %s", (_espHandshake) ? "ON" : "OFF");
		Debug(_log, "Delayed open  : %d", _delayAfterConnect_ms);

		isInitOK = true;
	}
	return isInitOK;
}

ProviderRs232::~ProviderRs232()
{
	if (_rs232Port.isOpen())
		_rs232Port.close();
}

int ProviderRs232::open()
{
	int retval = -1;
	_isDeviceReady = false;

	// open device physically
	if (tryOpen(_delayAfterConnect_ms))
	{
		// Everything is OK, device is ready
		_isDeviceReady = true;
		retval = 0;
	}
	return retval;
}

void ProviderRs232::waitForExitStats()
{
	if (_rs232Port.isOpen())
	{
		if (_rs232Port.bytesAvailable() > 16)
		{
			auto incoming = QString(_rs232Port.readAll());
			
			Info(_log, "Received: %s", QSTRING_CSTR(incoming));
		}
		if (!_isDeviceReady)
		{
			Debug(_log, "Close UART: %s", QSTRING_CSTR(_deviceName));
			_rs232Port.close();
		}
	}
}

int ProviderRs232::close()
{
	int retval = 0;

	_isDeviceReady = false;

	// Test, if device requires closing
	if (_rs232Port.isOpen())
	{
		if (_rs232Port.flush())
		{
			Debug(_log, "Flush was successful");
		}
		

		if (_espHandshake)
		{
			// read the statistics on goodbye
			QTimer::singleShot(6000, this, &ProviderRs232::waitForExitStats);
			connect(&_rs232Port, &QSerialPort::readyRead, this, &ProviderRs232::waitForExitStats);
		}
		else
		{
			Debug(_log, "Close UART: %s", QSTRING_CSTR(_deviceName));
			_rs232Port.close();
		}
		
	}
	return retval;
}

bool ProviderRs232::powerOff()
{
	// Simulate power-off by writing a final "Black" to have a defined outcome
	bool rc = false;
	if (writeBlack(NUM_POWEROFF_WRITE_BLACK) >= 0)
	{
		rc = true;
	}
	return rc;
}

bool ProviderRs232::tryOpen(int delayAfterConnect_ms)
{
	if (_deviceName.isEmpty() || _rs232Port.portName().isEmpty())
	{
		if (!_rs232Port.isOpen())
		{
			if (_isAutoDeviceName)
			{
				_deviceName = discoverFirst();
				if (_deviceName.isEmpty())
				{
					this->setInError(QString("No serial device found automatically!"));
					return false;
				}
			}
		}

		_rs232Port.setPortName(_deviceName);
	}

	if (!_rs232Port.isOpen())
	{
		Info(_log, "Opening UART: %s", QSTRING_CSTR(_deviceName));

		_frameDropCounter = 0;

		_rs232Port.setBaudRate(_baudRate_Hz);

		Debug(_log, "_rs232Port.open(QIODevice::ReadWrite): %s, Baud rate [%d]bps", QSTRING_CSTR(_deviceName), _baudRate_Hz);

		QSerialPortInfo serialPortInfo(_deviceName);

		QJsonObject portInfo;
		Debug(_log, "portName:          %s", QSTRING_CSTR(serialPortInfo.portName()));
		Debug(_log, "systemLocation:    %s", QSTRING_CSTR(serialPortInfo.systemLocation()));
		Debug(_log, "description:       %s", QSTRING_CSTR(serialPortInfo.description()));
		Debug(_log, "manufacturer:      %s", QSTRING_CSTR(serialPortInfo.manufacturer()));
		Debug(_log, "productIdentifier: %s", QSTRING_CSTR(QString("0x%1").arg(serialPortInfo.productIdentifier(), 0, 16)));
		Debug(_log, "vendorIdentifier:  %s", QSTRING_CSTR(QString("0x%1").arg(serialPortInfo.vendorIdentifier(), 0, 16)));
		Debug(_log, "serialNumber:      %s", QSTRING_CSTR(serialPortInfo.serialNumber()));

		if (!serialPortInfo.isNull())
		{
			if (!_rs232Port.isOpen() && !_rs232Port.open(QIODevice::ReadWrite))
			{
				this->setInError(_rs232Port.errorString());
				return false;
			}

			if (_espHandshake)
			{
				disconnect(&_rs232Port, &QSerialPort::readyRead, nullptr, nullptr);

				// reset to defaults				
				_rs232Port.setDataTerminalReady(true);
				_rs232Port.setRequestToSend(false);
				QThread::msleep(50);

				// reset device
				_rs232Port.setDataTerminalReady(false);
				_rs232Port.setRequestToSend(true);
				QThread::msleep(100);				

				// resume device
				_rs232Port.setRequestToSend(false);
				QThread::msleep(100);

				// read the reset message, search for AWA tag
				auto start = InternalClock::now();

				while(InternalClock::now() - start < 1000)
				{
					_rs232Port.waitForReadyRead(100);
					if (_rs232Port.bytesAvailable() > 16)
					{
						auto incoming = _rs232Port.readAll();
						for (int i = 0; i < incoming.length(); i++)
							if (!(incoming[i] == '\n' ||
								(incoming[i] >= ' ' && incoming[i] <= 'Z') ||
								(incoming[i] >= 'a' && incoming[i] <= 'z')))
							{
								incoming.replace(incoming[i], '*');
							}
						QString result = QString(incoming).remove('*').replace('\n',' ').trimmed();
						if (result.indexOf("Awa driver",Qt::CaseInsensitive) >= 0)
						{
							Info(_log, "DETECTED DEVICE USING HYPERSERIALESP8266/HYPERSERIALESP32 FIRMWARE (%s) at %i msec", QSTRING_CSTR(result), int(InternalClock::now() - start));
							start = 0;
							break;
						}						
					}
					if (InternalClock::now() <= start)
						break;
				}

				if (start != 0)
					Error(_log, "Could not detect HyperSerialEsp8266/HyperSerialESP32 device");
			}
		}
		else
		{
			QString errortext = QString("Invalid serial device name: [%1]!").arg(_deviceName);
			this->setInError(errortext);
			return false;
		}
	}

	if (delayAfterConnect_ms > 0)
	{

		Debug(_log, "delayAfterConnect for %d ms - start", delayAfterConnect_ms);

		// Wait delayAfterConnect_ms before allowing write
		QEventLoop loop;
		QTimer::singleShot(delayAfterConnect_ms, &loop, &QEventLoop::quit);
		loop.exec();

		Debug(_log, "delayAfterConnect for %d ms - finished", delayAfterConnect_ms);
	}

	return _rs232Port.isOpen();
}

void ProviderRs232::setInError(const QString& errorMsg)
{
	_rs232Port.clearError();
	this->close();

	LedDevice::setInError(errorMsg);
}

int ProviderRs232::writeBytes(const qint64 size, const uint8_t* data)
{
	int rc = 0;
	if (!_rs232Port.isOpen())
	{
		Debug(_log, "!_rs232Port.isOpen()");

		if (!tryOpen(OPEN_TIMEOUT.count()))
		{
			return -1;
		}
	}
	qint64 bytesWritten = _rs232Port.write(reinterpret_cast<const char*>(data), size);
	if (bytesWritten == -1 || bytesWritten != size)
	{
		this->setInError(QString("Rs232 SerialPortError: %1").arg(_rs232Port.errorString()));
		rc = -1;
	}
	else
	{
		if (!_rs232Port.waitForBytesWritten(WRITE_TIMEOUT.count()))
		{
			if (_rs232Port.error() == QSerialPort::TimeoutError)
			{
				Debug(_log, "Timeout after %dms: %d frames already dropped", WRITE_TIMEOUT, _frameDropCounter);

				++_frameDropCounter;

				// Check,if number of timeouts in a given time frame is greater than defined
				// TODO: ProviderRs232::writeBytes - Add time frame to check for timeouts that devices does not close after absolute number of timeouts
				if (_frameDropCounter > MAX_WRITE_TIMEOUTS)
				{
					this->setInError(QString("Timeout writing data to %1").arg(_deviceName));
					rc = -1;
				}
				else
				{
					//give it another try
					_rs232Port.clearError();
				}
			}
			else
			{
				this->setInError(QString("Rs232 SerialPortError: %1").arg(_rs232Port.errorString()));
				rc = -1;
			}
		}
	}
	return rc;
}

QString ProviderRs232::discoverFirst()
{
	for (int round = 0; round < 2; round++)
		for (auto const& port : QSerialPortInfo::availablePorts())
		{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
			if (!port.isNull() && !port.isBusy())
#else
			if (!port.isNull())
#endif
			{
				QString infoMessage = QString("%1 (%2 => %3)").arg(port.description()).arg(port.systemLocation()).arg(port.portName());

				if (round != 0 ||
					(port.description().contains("Bluetooth", Qt::CaseInsensitive) == false &&
						port.systemLocation().contains("ttyAMA0", Qt::CaseInsensitive) == false))
				{
					Info(_log, "Serial port auto-discovery. Found serial port device: %s", QSTRING_CSTR(infoMessage));
					return port.portName();
				}
				else
				{
					Warning(_log, "Serial port auto-discovery. Ignoring possible bluetooth device for now, try to find different available serial port: %s", QSTRING_CSTR(infoMessage));
				}
			}
		}
	return "";
}

QJsonObject ProviderRs232::discover(const QJsonObject& /*params*/)
{
	QJsonObject devicesDiscovered;
	QJsonArray deviceList;

	deviceList.push_back(QJsonObject{
			{"value", "auto"},
			{ "name", "Auto"} });

	for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts())
		deviceList.push_back(QJsonObject{
			{"value", info.portName()},
			{ "name", QString("%2 (%1)").arg(info.systemLocation()).arg(info.description()) } });

	devicesDiscovered.insert("ledDeviceType", _activeDeviceType);
	devicesDiscovered.insert("devices", deviceList);

	Debug(_log, "Serial devices discovered: [%s]", QString(QJsonDocument(devicesDiscovered).toJson(QJsonDocument::Compact)).toUtf8().constData());

	return devicesDiscovered;
}
