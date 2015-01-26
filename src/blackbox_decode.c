#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <math.h>
#include <ctype.h>

#include <errno.h>
#include <fcntl.h>

//For msvcrt to define M_PI:
#define _USE_MATH_DEFINES
#include <math.h>

#ifdef WIN32
    #include <io.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef WIN32
    #include "getopt.h"
#else
    #include <getopt.h>
#endif

#include "parser.h"
#include "platform.h"
#include "tools.h"
#include "gpxwriter.h"
#include "imu.h"

typedef enum Unit {
    UNIT_RAW = 0,
    UNIT_METERS_PER_SECOND,
    UNIT_KILOMETERS_PER_HOUR,
    UNIT_MILES_PER_HOUR,
    UNIT_MILLIVOLTS,
    UNIT_MILLIAMPS,
    UNIT_VOLTS,
    UNIT_AMPS
} Unit;

static const char* const UNIT_NAME[] = {
    "raw",
    "m/s",
    "km/h",
    "mi/h",
    "mV",
    "mA",
    "V",
    "A"
};

typedef struct decodeOptions_t {
    int help, raw, limits, debug, toStdout;
    int logNumber;
    int simulateIMU, imuIgnoreMag;
    int mergeGPS;
    const char *outputPrefix;

    Unit unitGPSSpeed, unitVbat, unitAmperage;
} decodeOptions_t;

decodeOptions_t options = {
    .help = 0, .raw = 0, .limits = 0, .debug = 0, .toStdout = 0,
    .logNumber = -1,
    .simulateIMU = false, .imuIgnoreMag = 0,
    .mergeGPS = 0,

    .outputPrefix = NULL,

    .unitGPSSpeed = UNIT_METERS_PER_SECOND,
    .unitVbat = UNIT_VOLTS,
    .unitAmperage = UNIT_AMPS
};

//We'll use field names to identify GPS field units so the values can be formatted for display
typedef enum {
    GPS_FIELD_TYPE_INTEGER,
    GPS_FIELD_TYPE_DEGREES_TIMES_10, // for headings
    GPS_FIELD_TYPE_COORDINATE_DEGREES_TIMES_10000000,
    GPS_FIELD_TYPE_METERS_PER_SECOND_TIMES_100,
    GPS_FIELD_TYPE_METERS
} GPSFieldType;

static GPSFieldType gpsFieldTypes[FLIGHT_LOG_MAX_FIELDS];

static uint32_t lastFrameTime = (uint32_t) -1;

static FILE *csvFile = 0, *eventFile = 0, *gpsCsvFile = 0;
static char *eventFilename = 0, *gpsCsvFilename = 0;
static gpxWriter_t *gpx = 0;

static attitude_t attitude;

static Unit mainFieldUnit[FLIGHT_LOG_MAX_FIELDS];
static Unit gpsGFieldUnit[FLIGHT_LOG_MAX_FIELDS];

static int32_t bufferedMainFrame[FLIGHT_LOG_MAX_FIELDS];
static bool haveBufferedMainFrame;

static uint32_t bufferedFrameTime;

static int32_t bufferedGPSFrame[FLIGHT_LOG_MAX_FIELDS];

void onEvent(flightLog_t *log, flightLogEvent_t *event)
{
    (void) log;

    // Open the event log if it wasn't open already
    if (!eventFile) {
        if (eventFilename) {
            eventFile = fopen(eventFilename, "wb");

            if (!eventFile) {
                fprintf(stderr, "Failed to create event log file %s\n", eventFilename);
                return;
            }
        } else {
            //Nowhere to log
            return;
        }
    }

    switch (event->event) {
        case FLIGHT_LOG_EVENT_SYNC_BEEP:
            fprintf(eventFile, "{name:\"Sync beep\", time:%u}\n", event->data.syncBeep.time);
        break;
        case FLIGHT_LOG_EVENT_AUTOTUNE_CYCLE_START:
            fprintf(eventFile, "{name:\"Autotune cycle start\", time:%u, data:{phase:%d,cycle:%d,p:%u,i:%u,d:%u,rising:%d}}\n", lastFrameTime,
                event->data.autotuneCycleStart.phase, event->data.autotuneCycleStart.cycle & 0x7F /* Top bit used for "rising: */,
                event->data.autotuneCycleStart.p, event->data.autotuneCycleStart.i, event->data.autotuneCycleStart.d,
                event->data.autotuneCycleStart.cycle >> 7);
        break;
        case FLIGHT_LOG_EVENT_AUTOTUNE_CYCLE_RESULT:
            fprintf(eventFile, "{name:\"Autotune cycle result\", time:%u, data:{overshot:%s,timedout:%s,p:%u,i:%u,d:%u}}\n", lastFrameTime,
                event->data.autotuneCycleResult.flags & FLIGHT_LOG_EVENT_AUTOTUNE_FLAG_OVERSHOT ? "true" : "false",
                event->data.autotuneCycleResult.flags & FLIGHT_LOG_EVENT_AUTOTUNE_FLAG_TIMEDOUT ? "true" : "false",
                event->data.autotuneCycleResult.p, event->data.autotuneCycleResult.i, event->data.autotuneCycleResult.d);
        break;
        case FLIGHT_LOG_EVENT_AUTOTUNE_TARGETS:
            fprintf(eventFile, "{name:\"Autotune cycle targets\", time:%u, data:{currentAngle:%.1f,targetAngle:%d,targetAngleAtPeak:%d,firstPeakAngle:%.1f,secondPeakAngle:%.1f}}\n", lastFrameTime,
                event->data.autotuneTargets.currentAngle / 10.0,
                event->data.autotuneTargets.targetAngle, event->data.autotuneTargets.targetAngleAtPeak,
                event->data.autotuneTargets.firstPeakAngle / 10.0, event->data.autotuneTargets.secondPeakAngle / 10.0);
        break;
        case FLIGHT_LOG_EVENT_LOG_END:
            fprintf(eventFile, "{name:\"Log clean end\", time:%u}\n", lastFrameTime);
        break;
        default:
            fprintf(eventFile, "{name:\"Unknown event\", time:%u, data:{eventID:%d}}\n", lastFrameTime, event->event);
        break;
    }
}

/**
 * Print out a comma separated list of GPS field names, minus the time field.
 */
void outputGPSFieldNamesHeader(flightLog_t *log, FILE *file)
{
    bool needComma = false;

    for (int i = 0; i < log->gpsFieldCount; i++) {
        if (i == log->gpsFieldIndexes.time)
            continue;

        if (needComma) {
            fprintf(file, ", ");
        } else {
            needComma = true;
        }

        fprintf(file, "%s", log->gpsFieldNames[i]);

        if (gpsGFieldUnit[i] != UNIT_RAW) {
            fprintf(file, " (%s)", UNIT_NAME[gpsGFieldUnit[i]]);
        }
    }
}

/**
 * Attempt to create a file to log GPS data in CSV format. On success, gpsCsvFile is non-NULL.
 */
void createGPSCSVFile(flightLog_t *log)
{
    if (!gpsCsvFile && gpsCsvFilename) {
        gpsCsvFile = fopen(gpsCsvFilename, "wb");

        if (gpsCsvFile) {
            fprintf(gpsCsvFile, "time, ");

            outputGPSFieldNamesHeader(log, gpsCsvFile);

            fprintf(gpsCsvFile, "\n");
        }
    }
}

double convertMetersPerSecondToUnit(double meterspersec, Unit unit)
{
    static const double MILES_PER_METER = 0.00062137;

    switch (unit) {
        case UNIT_KILOMETERS_PER_HOUR:
            return meterspersec * 60 * 60 / 1000;
        break;

        case UNIT_MILES_PER_HOUR:
            return meterspersec * MILES_PER_METER * 60 * 60;
        break;

        case UNIT_METERS_PER_SECOND:
            return meterspersec;

        case UNIT_RAW:
            fprintf(stderr, "Attempted to convert speed to raw units but this data is already cooked\n");
            exit(-1);
        break;
        default:
            fprintf(stderr, "Bad speed unit in conversion\n");
            exit(-1);
    }
}

static void updateIMU(flightLog_t *log, int32_t *frame, uint32_t currentTime, attitude_t *result)
{
    int16_t gyroData[3];
    int16_t accSmooth[3];
    int16_t magADC[3];
    bool hasMag = log->mainFieldIndexes.magADC[0] > -1;

    int i;

    for (i = 0; i < 3; i++) {
        gyroData[i] = (int16_t) frame[log->mainFieldIndexes.gyroData[i]];
        accSmooth[i] = (int16_t) frame[log->mainFieldIndexes.accSmooth[i]];
    }

    if (hasMag && !options.imuIgnoreMag) {
        for (i = 0; i < 3; i++) {
            magADC[i] = (int16_t) frame[log->mainFieldIndexes.magADC[i]];
        }
    }

    updateEstimatedAttitude(gyroData, accSmooth, hasMag && !options.imuIgnoreMag ? magADC : NULL,
        currentTime, log->sysConfig.acc_1G, log->sysConfig.gyroScale, result);
}

/**
 * Print the GPS fields from the given GPS frame as comma-separated values (the GPS frame time is not printed).
 */
void outputGPSFields(flightLog_t *log, FILE *file, int32_t *frame)
{
    int i;
    int32_t degrees;
    uint32_t fracDegrees;
    bool needComma = false;

    for (i = 0; i < log->gpsFieldCount; i++) {
        //We've already printed the time:
        if (i == log->gpsFieldIndexes.time)
            continue;

        if (needComma)
            fprintf(file, ", ");
        else
            needComma = true;

        switch (gpsFieldTypes[i]) {
            case GPS_FIELD_TYPE_COORDINATE_DEGREES_TIMES_10000000:
                degrees = frame[i] / 10000000;
                fracDegrees = abs(frame[i]) % 10000000;

                fprintf(file, "%d.%07u", degrees, fracDegrees);
            break;
            case GPS_FIELD_TYPE_DEGREES_TIMES_10:
                fprintf(file, "%d.%01u", frame[i] / 10, abs(frame[i]) % 10);
            break;
            case GPS_FIELD_TYPE_METERS_PER_SECOND_TIMES_100:
                if (options.unitGPSSpeed == UNIT_RAW) {
                    fprintf(file, "%d", frame[i]);
                } else if (options.unitGPSSpeed == UNIT_METERS_PER_SECOND) {
                    fprintf(file, "%d.%02u", frame[i] / 100, abs(frame[i]) % 100);
                } else {
                    fprintf(file, "%.2f", convertMetersPerSecondToUnit(frame[i] / 100.0, options.unitGPSSpeed));
                }
            break;
            case GPS_FIELD_TYPE_METERS:
                fprintf(file, "%d", frame[i]);
            break;
            case GPS_FIELD_TYPE_INTEGER:
            default:
                fprintf(file, "%d", frame[i]);
        }
    }
}

void outputGPSFrame(flightLog_t *log, int32_t *frame)
{
    uint32_t gpsFrameTime;

    // If we're not logging every loop iteration, we include a timestamp field in the GPS frame:
    if (log->gpsFieldIndexes.time != -1) {
        gpsFrameTime = frame[log->gpsFieldIndexes.time];
    } else {
        // Otherwise this GPS frame was recorded at the same time as the main stream frame we read before the GPS frame:
        gpsFrameTime = lastFrameTime;
    }

    // We need at least lat/lon/altitude from the log to write a useful GPX track
    if (log->gpsFieldIndexes.GPS_coord[0] != -1 && log->gpsFieldIndexes.GPS_coord[0] != -1 && log->gpsFieldIndexes.GPS_altitude != -1) {
        gpxWriterAddPoint(gpx, lastFrameTime, frame[log->gpsFieldIndexes.GPS_coord[0]], frame[log->gpsFieldIndexes.GPS_coord[1]], frame[log->gpsFieldIndexes.GPS_altitude]);
    }

    createGPSCSVFile(log);

    if (gpsCsvFile) {
        fprintf(gpsCsvFile, "%u, ", gpsFrameTime);

        outputGPSFields(log, gpsCsvFile, frame);

        fprintf(gpsCsvFile, "\n");
    }
}

/**
 * Print out the fields from the main log stream in comma separated format.
 *
 * Provide (uint32_t) -1 for the frameTime in order to mark the frame time as unknown.
 */
void outputMainFrameFields(flightLog_t *log, uint32_t frameTime, int32_t *frame)
{
    int i;
    bool needComma = false;

    for (i = 0; i < log->mainFieldCount; i++) {
        if (needComma) {
            fprintf(csvFile, ", ");
        } else {
            needComma = true;
        }

        if (i == FLIGHT_LOG_FIELD_INDEX_TIME) {
            // Use the time the caller provided instead of the time in the frame
            if (frameTime == (uint32_t) -1)
                fprintf(csvFile, "X");
            else
                fprintf(csvFile, "%u", frameTime);
        } else {
            switch (mainFieldUnit[i]) {
                case UNIT_VOLTS:
                    if (i != log->mainFieldIndexes.vbatLatest) {
                        fprintf(stderr, "Bad unit for field %d\n", i);
                        exit(-1);
                    }

                    fprintf(csvFile, "%.3f", flightLogVbatADCToMillivolts(log, (uint16_t)frame[i]) / 1000.0);
                break;
                case UNIT_AMPS:
                    if (i != log->mainFieldIndexes.amperageLatest) {
                        fprintf(stderr, "Bad unit for field %d\n", i);
                        exit(-1);
                    }

                    fprintf(csvFile, "%.3f", flightLogAmperageADCToMilliamps(log, (uint16_t)frame[i]) / 1000.0);
                break;            
                case UNIT_MILLIVOLTS:
                    if (i != log->mainFieldIndexes.vbatLatest) {
                        fprintf(stderr, "Bad unit for field %d\n", i);
                        exit(-1);
                    }

                    fprintf(csvFile, "%u", flightLogVbatADCToMillivolts(log, (uint16_t)frame[i]));
                break;
                case UNIT_MILLIAMPS:
                    if (i != log->mainFieldIndexes.amperageLatest) {
                        fprintf(stderr, "Bad unit for field %d\n", i);
                        exit(-1);
                    }

                    fprintf(csvFile, "%u", flightLogAmperageADCToMilliamps(log, (uint16_t)frame[i]));
                break;
                default:
                case UNIT_RAW:
                    if (log->mainFieldSigned[i] || options.raw)
                        fprintf(csvFile, "%3d", frame[i]);
                    else
                        fprintf(csvFile, "%3u", (uint32_t) frame[i]);
                break;
            }
        }
    }

    if (options.simulateIMU) {
        fprintf(csvFile, ", %.2f, %.2f, %.2f", attitude.roll * 180 / M_PI, attitude.pitch * 180 / M_PI, attitude.heading * 180 / M_PI);
    }
}

void outputMergeFrame(flightLog_t *log)
{
    outputMainFrameFields(log, bufferedFrameTime, bufferedMainFrame);
    fprintf(csvFile, ", ");
    outputGPSFields(log, csvFile, bufferedGPSFrame);
    fprintf(csvFile, "\n");

    haveBufferedMainFrame = false;
}

/**
 * This is called when outputting the log in GPS merge mode. When we parse a main frame, we don't know if a GPS frame
 * exists at the same frame time yet, so we we buffer up the main frame data to print later until we know for sure.
 *
 * We also keep a copy of the GPS frame data so we can print it out multiple times if multiple main frames arrive
 * between GPS updates.
 */
void onFrameReadyMerge(flightLog_t *log, bool frameValid, int32_t *frame, uint8_t frameType, int fieldCount, int frameOffset, int frameSize)
{
    uint32_t gpsFrameTime;

    (void) frameOffset;
    (void) frameSize;

    if (frameType == 'G') {
        if (frameValid) {
            if (log->gpsFieldIndexes.time == -1 || (uint32_t) frame[log->gpsFieldIndexes.time] == lastFrameTime) {
                //This GPS frame was logged in the same iteration as the main frame that preceded it
                gpsFrameTime = lastFrameTime;
            } else {
                gpsFrameTime = frame[log->gpsFieldIndexes.time];

                /*
                 * This GPS frame happened some time after the main frame that preceded it, so print out that main
                 * frame with its older timestamp first if we didn't print it already.
                 */
                if (haveBufferedMainFrame) {
                    outputMergeFrame(log);
                }
            }

            /*
             * Copy this GPS data for later since we may need to duplicate it if there is another main frame before
             * we get another GPS update.
             */
            memcpy(bufferedGPSFrame, frame, sizeof(*bufferedGPSFrame) * fieldCount);
            bufferedFrameTime = gpsFrameTime;

            outputMergeFrame(log);

            // We need at least lat/lon/altitude from the log to write a useful GPX track
            if (log->gpsFieldIndexes.GPS_coord[0] != -1 && log->gpsFieldIndexes.GPS_coord[0] != -1 && log->gpsFieldIndexes.GPS_altitude != -1) {
                gpxWriterAddPoint(gpx, gpsFrameTime, frame[log->gpsFieldIndexes.GPS_coord[0]], frame[log->gpsFieldIndexes.GPS_coord[1]], frame[log->gpsFieldIndexes.GPS_altitude]);
            }
        }
    } else if (frameType == 'P' || frameType == 'I') {
        if (frameValid || (frame && options.raw)) {
            if (haveBufferedMainFrame) {
                outputMergeFrame(log);
            }

            if (frameValid) {
                lastFrameTime = (uint32_t) frame[FLIGHT_LOG_FIELD_INDEX_TIME];
            }

            if (options.simulateIMU) {
                updateIMU(log, frame, lastFrameTime, &attitude);
            }

            /*
             * Store this frame to print out later since we don't know if a GPS frame follows it yet.
             */
            memcpy(bufferedMainFrame, frame, sizeof(*bufferedMainFrame) * fieldCount);

            if (frameValid) {
                bufferedFrameTime = lastFrameTime;
            } else {
                bufferedFrameTime = -1;
            }

            haveBufferedMainFrame = true;
        }
    }
}

void onFrameReady(flightLog_t *log, bool frameValid, int32_t *frame, uint8_t frameType, int fieldCount, int frameOffset, int frameSize)
{
    if (options.mergeGPS && log->gpsFieldCount > 0) {
        //Use the alternate frame processing routine which merges main stream data and GPS data together
        onFrameReadyMerge(log, frameValid, frame, frameType, fieldCount, frameOffset, frameSize);
        return;
    }

    if (frameType == 'G') {
        if (frameValid) {
            outputGPSFrame(log, frame);
        }
    } else if (frameType == 'P' || frameType == 'I') {
        if (frameValid || (frame && options.raw)) {
            lastFrameTime = (uint32_t) frame[FLIGHT_LOG_FIELD_INDEX_TIME];

            if (options.simulateIMU) {
                updateIMU(log, frame, lastFrameTime, &attitude);
            }

            outputMainFrameFields(log, frameValid ? (uint32_t) frame[FLIGHT_LOG_FIELD_INDEX_TIME] : (uint32_t) -1, frame);

            if (options.debug) {
                fprintf(csvFile, ", %c, offset %d, size %d\n", (char) frameType, frameOffset, frameSize);
            } else
                fprintf(csvFile, "\n");
        } else if (options.debug) {
            // Print to stdout so that these messages line up with our other output on stdout (stderr isn't synchronised to it)
            if (frame) {
                /*
                 * We'll assume that the frame's iteration count is still fairly sensible (if an earlier frame was corrupt,
                 * the frame index will be smaller than it should be)
                 */
                fprintf(csvFile, "%c Frame unusuable due to prior corruption, offset %d, size %d\n", (char) frameType, frameOffset, frameSize);
            } else {
                fprintf(csvFile, "Failed to decode %c frame, offset %d, size %d\n", (char) frameType, frameOffset, frameSize);
            }
        }
    }
}

void resetGPSFieldIdents()
{
    for (int i = 0; i < FLIGHT_LOG_MAX_FIELDS; i++)
        gpsFieldTypes[i] = GPS_FIELD_TYPE_INTEGER;
}

/**
 * Sets the units/display format we should use for each GPS field into the global `gpsFieldTypes`.
 */
void identifyGPSFields(flightLog_t *log)
{
    int i;

    for (i = 0; i < log->gpsFieldCount; i++) {
        const char *fieldName = log->gpsFieldNames[i];

        if (strcmp(fieldName, "GPS_coord[0]") == 0) {
            gpsFieldTypes[i] = GPS_FIELD_TYPE_COORDINATE_DEGREES_TIMES_10000000;
        } else if (strcmp(fieldName, "GPS_coord[1]") == 0) {
            gpsFieldTypes[i] = GPS_FIELD_TYPE_COORDINATE_DEGREES_TIMES_10000000;
        } else if (strcmp(fieldName, "GPS_altitude") == 0) {
            gpsFieldTypes[i] = GPS_FIELD_TYPE_METERS;
        } else if (strcmp(fieldName, "GPS_speed") == 0) {
            gpsFieldTypes[i] = GPS_FIELD_TYPE_METERS_PER_SECOND_TIMES_100;
        } else if (strcmp(fieldName, "GPS_ground_course") == 0) {
            gpsFieldTypes[i] = GPS_FIELD_TYPE_DEGREES_TIMES_10;
        } else {
            gpsFieldTypes[i] = GPS_FIELD_TYPE_INTEGER;
        }
    }
}

/**
 * After reading in what fields are present, this routine is called in order to apply the user's
 * commandline choices for field units to the global "mainFieldUnit" and "gpsGFieldUnit" arrays.
 */
void applyFieldUnits(flightLog_t *log)
{
    memset(mainFieldUnit, 0, sizeof(mainFieldUnit));
    memset(gpsGFieldUnit, 0, sizeof(gpsGFieldUnit));

    if (log->mainFieldIndexes.vbatLatest > -1) {
        mainFieldUnit[log->mainFieldIndexes.vbatLatest] = options.unitVbat;
    }
    
    if (log->mainFieldIndexes.amperageLatest > -1) {
        mainFieldUnit[log->mainFieldIndexes.amperageLatest] = options.unitAmperage;
    }

    if (log->gpsFieldIndexes.GPS_speed > -1) {
        mainFieldUnit[log->gpsFieldIndexes.GPS_speed] = options.unitGPSSpeed;
    }
}

void writeMainCSVHeader(flightLog_t *log)
{
    int i;

    for (i = 0; i < log->mainFieldCount; i++) {
        if (i > 0)
            fprintf(csvFile, ", ");

        fprintf(csvFile, "%s", log->mainFieldNames[i]);

        if (mainFieldUnit[i] != UNIT_RAW) {
            fprintf(csvFile, " (%s)", UNIT_NAME[mainFieldUnit[i]]);
        }
    }

    if (options.simulateIMU) {
        fprintf(csvFile, ", roll, pitch, heading");
    }

    if (options.mergeGPS && log->gpsFieldCount > 0) {
        fprintf(csvFile, ", ");

        outputGPSFieldNamesHeader(log, csvFile);
    }

    fprintf(csvFile, "\n");
}

void onMetadataReady(flightLog_t *log)
{
    if (log->mainFieldCount == 0) {
        fprintf(stderr, "No fields found in log, is it missing its header?\n");
        return;
    } else if (options.simulateIMU && (log->mainFieldIndexes.accSmooth[0] == -1 || log->mainFieldIndexes.gyroData[0] == -1)){
        fprintf(stderr, "Can't simulate the IMU because accelerometer or gyroscope data is missing\n");
        options.simulateIMU = false;
    }

    identifyGPSFields(log);
    applyFieldUnits(log);

    writeMainCSVHeader(log);
}

void printStats(flightLog_t *log, int logIndex, bool raw, bool limits)
{
    flightLogStatistics_t *stats = &log->stats;
    uint32_t intervalMS = (uint32_t) ((stats->field[FLIGHT_LOG_FIELD_INDEX_TIME].max - stats->field[FLIGHT_LOG_FIELD_INDEX_TIME].min) / 1000);

    uint32_t goodBytes = stats->frame['I'].bytes + stats->frame['P'].bytes;
    uint32_t goodFrames = stats->frame['I'].validCount + stats->frame['P'].validCount;
    uint32_t totalFrames = (uint32_t) (stats->field[FLIGHT_LOG_FIELD_INDEX_ITERATION].max - stats->field[FLIGHT_LOG_FIELD_INDEX_ITERATION].min + 1);
    int32_t missingFrames = totalFrames - goodFrames - stats->intentionallyAbsentIterations;

    uint32_t runningTimeMS, runningTimeSecs, runningTimeMins;
    uint32_t startTimeMS, startTimeSecs, startTimeMins;
    uint32_t endTimeMS, endTimeSecs, endTimeMins;

    uint8_t frameTypes[] = {'I', 'P', 'H', 'G', 'E'};

    int i;

    if (missingFrames < 0)
        missingFrames = 0;

    runningTimeMS = intervalMS;
    runningTimeSecs = runningTimeMS / 1000;
    runningTimeMS %= 1000;
    runningTimeMins = runningTimeSecs / 60;
    runningTimeSecs %= 60;

    startTimeMS = (uint32_t) (stats->field[FLIGHT_LOG_FIELD_INDEX_TIME].min / 1000);
    startTimeSecs = startTimeMS / 1000;
    startTimeMS %= 1000;
    startTimeMins = startTimeSecs / 60;
    startTimeSecs %= 60;

    endTimeMS = (uint32_t) (stats->field[FLIGHT_LOG_FIELD_INDEX_TIME].max / 1000);
    endTimeSecs = endTimeMS / 1000;
    endTimeMS %= 1000;
    endTimeMins = endTimeSecs / 60;
    endTimeSecs %= 60;

    fprintf(stderr, "\nLog %d of %d", logIndex + 1, log->logCount);

    if (intervalMS > 0 && !raw) {
        fprintf(stderr, ", start %02d:%02d.%03d, end %02d:%02d.%03d, duration %02d:%02d.%03d\n\n",
            startTimeMins, startTimeSecs, startTimeMS,
            endTimeMins, endTimeSecs, endTimeMS,
            runningTimeMins, runningTimeSecs, runningTimeMS
        );
    }

    fprintf(stderr, "Statistics\n");

    for (i = 0; i < (int) sizeof(frameTypes); i++) {
        uint8_t frameType = frameTypes[i];

        if (stats->frame[frameType].validCount ) {
            fprintf(stderr, "%c frames %7d %6.1f bytes avg %8d bytes total\n", (char) frameType, stats->frame[frameType].validCount,
                (float) stats->frame[frameType].bytes / stats->frame[frameType].validCount, stats->frame[frameType].bytes);
        }
    }

    if (goodFrames) {
        fprintf(stderr, "Frames %9d %6.1f bytes avg %8d bytes total\n", goodFrames, (float) goodBytes / goodFrames, goodBytes);
    } else {
        fprintf(stderr, "Frames %8d\n", 0);
    }

    if (intervalMS > 0 && !raw) {
        fprintf(stderr, "Data rate %4uHz %6u bytes/s %10u baud\n",
            (unsigned int) (((int64_t) goodFrames * 1000) / intervalMS),
            (unsigned int) (((int64_t) stats->totalBytes * 1000) / intervalMS),
            (unsigned int) ((((int64_t) stats->totalBytes * 1000 * 8) / intervalMS + 100 - 1) / 100 * 100)); /* Round baud rate up to nearest 100 */
    } else {
        fprintf(stderr, "Data rate: Unknown, no timing information available.\n");
    }

    if (totalFrames && (stats->totalCorruptFrames || missingFrames || stats->intentionallyAbsentIterations)) {
        fprintf(stderr, "\n");

        if (stats->totalCorruptFrames || stats->frame['P'].desyncCount || stats->frame['I'].desyncCount) {
            fprintf(stderr, "%d frames failed to decode, rendering %d loop iterations unreadable. ", stats->totalCorruptFrames, stats->frame['P'].desyncCount + stats->frame['P'].corruptCount + stats->frame['I'].desyncCount + stats->frame['I'].corruptCount);
            if (!missingFrames)
                fprintf(stderr, "\n");
        }
        if (missingFrames) {
            fprintf(stderr, "%d iterations are missing in total (%ums, %.2f%%)\n",
                missingFrames,
                (unsigned int) (((int64_t) missingFrames * intervalMS) / totalFrames),
                (double) missingFrames / totalFrames * 100);
        }
        if (stats->intentionallyAbsentIterations) {
            fprintf(stderr, "%d loop iterations weren't logged because of your blackbox_rate settings (%ums, %.2f%%)\n",
                stats->intentionallyAbsentIterations,
                (unsigned int) (((int64_t)stats->intentionallyAbsentIterations * intervalMS) / totalFrames),
                (double) stats->intentionallyAbsentIterations / totalFrames * 100);
        }
    }

    if (limits) {
        fprintf(stderr, "\n\n    Field name          Min          Max        Range\n");
        fprintf(stderr,     "-----------------------------------------------------\n");

        for (i = 0; i < log->mainFieldCount; i++) {
            fprintf(stderr, "%14s %12" PRId64 " %12" PRId64 " %12" PRId64 "\n",
                log->mainFieldNames[i],
                stats->field[i].min,
                stats->field[i].max,
                stats->field[i].max - stats->field[i].min
            );
        }
    }

    fprintf(stderr, "\n");
}

int decodeFlightLog(flightLog_t *log, const char *filename, int logIndex)
{
    // Organise output files/streams
    gpx = NULL;

    gpsCsvFile = NULL;
    gpsCsvFilename = NULL;

    eventFile = NULL;
    eventFilename = NULL;

    if (options.toStdout) {
        csvFile = stdout;
    } else {
        char *csvFilename = 0, *gpxFilename = 0;
        int filenameLen;

        const char *outputPrefix = 0;
        int outputPrefixLen;

        if (options.outputPrefix) {
            outputPrefix = options.outputPrefix;
            outputPrefixLen = strlen(options.outputPrefix);
        } else {
            const char *fileExtensionPeriod = strrchr(filename, '.');
            const char *logNameEnd;

            if (fileExtensionPeriod) {
                logNameEnd = fileExtensionPeriod;
            } else {
                logNameEnd = filename + strlen(filename);
            }

            outputPrefix = filename;
            outputPrefixLen = logNameEnd - outputPrefix;
        }

        filenameLen = outputPrefixLen + strlen(".00.csv") + 1;
        csvFilename = malloc(filenameLen * sizeof(char));

        snprintf(csvFilename, filenameLen, "%.*s.%02d.csv", outputPrefixLen, outputPrefix, logIndex + 1);

        filenameLen = outputPrefixLen + strlen(".00.gps.gpx") + 1;
        gpxFilename = malloc(filenameLen * sizeof(char));

        snprintf(gpxFilename, filenameLen, "%.*s.%02d.gps.gpx", outputPrefixLen, outputPrefix, logIndex + 1);

        filenameLen = outputPrefixLen + strlen(".00.gps.csv") + 1;
        gpsCsvFilename = malloc(filenameLen * sizeof(char));

        snprintf(gpsCsvFilename, filenameLen, "%.*s.%02d.gps.csv", outputPrefixLen, outputPrefix, logIndex + 1);

        filenameLen = outputPrefixLen + strlen(".00.event") + 1;
        eventFilename = malloc(filenameLen * sizeof(char));

        snprintf(eventFilename, filenameLen, "%.*s.%02d.event", outputPrefixLen, outputPrefix, logIndex + 1);

        csvFile = fopen(csvFilename, "wb");

        if (!csvFile) {
            fprintf(stderr, "Failed to create output file %s\n", csvFilename);

            free(csvFilename);
            return -1;
        }

        fprintf(stderr, "Decoding log '%s' to '%s'...\n", filename, csvFilename);
        free(csvFilename);

        gpx = gpxWriterCreate(gpxFilename);
        free(gpxFilename);
    }

    if (options.simulateIMU) {
        imuInit();
    }

    if (options.mergeGPS) {
        haveBufferedMainFrame = false;
        bufferedFrameTime = (uint32_t) -1;
        memset(bufferedGPSFrame, 0, sizeof(bufferedGPSFrame));
        memset(bufferedMainFrame, 0, sizeof(bufferedMainFrame));
    }

    int success = flightLogParse(log, logIndex, onMetadataReady, onFrameReady, onEvent, options.raw);

    if (options.mergeGPS && haveBufferedMainFrame) {
        // Print out last log entry that wasn't already printed
        outputMergeFrame(log);
    }

    if (success)
        printStats(log, logIndex, options.raw, options.limits);

    if (!options.toStdout)
        fclose(csvFile);

    free(eventFilename);
    if (eventFile)
        fclose(eventFile);

    free(gpsCsvFilename);
    if (gpsCsvFile)
        fclose(gpsCsvFile);

    gpxWriterDestroy(gpx);

    return success ? 0 : -1;
}

int validateLogIndex(flightLog_t *log)
{
    //Did the user pick a log to render?
    if (options.logNumber > 0) {
        if (options.logNumber > log->logCount) {
            fprintf(stderr, "Couldn't load log #%d from this file, because there are only %d logs in total.\n", options.logNumber, log->logCount);
            return -1;
        }

        return options.logNumber - 1;
    } else if (log->logCount == 1) {
        // If there's only one log, just parse that
        return 0;
    } else {
        fprintf(stderr, "This file contains multiple flight logs, please choose one with the --index argument:\n\n");

        fprintf(stderr, "Index  Start offset  Size (bytes)\n");
        for (int i = 0; i < log->logCount; i++) {
            fprintf(stderr, "%5d %13d %13d\n", i + 1, (int) (log->logBegin[i] - log->logBegin[0]), (int) (log->logBegin[i + 1] - log->logBegin[i]));
        }

        return -1;
    }
}

void printUsage(const char *argv0)
{
    fprintf(stderr,
        "Blackbox flight log decoder by Nicholas Sherlock ("
#ifdef BLACKBOX_VERSION
            "v" STR(BLACKBOX_VERSION) ", "
#endif
            __DATE__ " " __TIME__ ")\n\n"
        "Usage:\n"
        "     %s [options] <input logs>\n\n"
        "Options:\n"
        "   --help                   This page\n"
        "   --index <num>            Choose the log from the file that should be decoded (or omit to decode all)\n"
        "   --limits                 Print the limits and range of each field\n"
        "   --stdout                 Write log to stdout instead of to a file\n"
        "   --unit-gps-speed <unit>  GPS speed unit (mps|kph|mph), default is mps (meters per second)\n"
        "   --unit-amperage <unit>   Current meter unit (raw|mA|A), default is A (amps)\n"
        "   --unit-vbat <unit>       Vbat unit (raw|mV|V), default is V (volts)\n"
        "   --merge-gps              Merge GPS data into the main CSV log file instead of writing it separately\n"
        "   --simulate-imu           Compute tilt/roll/heading fields from gyro/accel/mag data\n"
        "   --imu-ignore-mag         Ignore magnetometer data when computing heading\n"
        "   --declination <val>      Set magnetic declination in degrees.minutes format (e.g. -12.58 for New York)\n"
        "   --declination-dec <val>  Set magnetic declination in decimal degrees (e.g. -12.97 for New York)\n"
        "   --debug                  Show extra debugging information\n"
        "   --raw                    Don't apply predictions to fields (show raw field deltas)\n"
        "\n", argv0
    );
}

/**
 * Case-insentive string equality test.
 */
bool striequals(const char *first, const char *second)
{
    while (1) {
        if (tolower(*first) != tolower(*second)) {
            return false;
        }
        if (*first == '\0') {
            return true;
        }

        first++;
        second++;
    }
}

bool parseUnit(const char *text, Unit *unit)
{
    if (striequals(text, "kph") || striequals(text, "kmph")  || striequals(text, "km/h") || striequals(text, "km/hr")) {
        *unit = UNIT_KILOMETERS_PER_HOUR;
    } else if (striequals(text, "mps") || striequals(text, "m/s")) {
        *unit = UNIT_METERS_PER_SECOND;
    } else if (striequals(text, "mph") || striequals(text, "mi/h") || striequals(text, "mi/hr")) {
        *unit = UNIT_MILES_PER_HOUR;
    } else if (striequals(text, "mv")) {
        *unit = UNIT_MILLIVOLTS;
    } else if (striequals(text, "ma")) {
        *unit = UNIT_MILLIAMPS;
    } else if (striequals(text, "v")) {
        *unit = UNIT_VOLTS;
    } else if (striequals(text, "a")) {
        *unit = UNIT_AMPS;
    } else if (striequals(text, "raw")) {
        *unit = UNIT_RAW;
    } else {
        return false;
    }

    return true;
}

double parseDegreesMinutes(const char *s)
{
    int combined = (int) round(atof(s) * 100);

    int degrees = combined / 100;
    int minutes = combined % 100;

    return degrees + (double) minutes / 60;
}

void parseCommandlineOptions(int argc, char **argv)
{
    int c;

    enum {
        SETTING_PREFIX = 1,
        SETTING_INDEX,
        SETTING_DECLINATION,
        SETTING_DECLINATION_DECIMAL,
        SETTING_UNIT_GPS_SPEED,
        SETTING_UNIT_VBAT,
        SETTING_UNIT_AMPERAGE
    };

    while (1)
    {
        static struct option long_options[] = {
            {"help", no_argument, &options.help, 1},
            {"raw", no_argument, &options.raw, 1},
            {"debug", no_argument, &options.debug, 1},
            {"limits", no_argument, &options.limits, 1},
            {"stdout", no_argument, &options.toStdout, 1},
            {"merge-gps", no_argument, &options.mergeGPS, 1},
            {"simulate-imu", no_argument, &options.simulateIMU, 1},
            {"imu-ignore-mag", no_argument, &options.imuIgnoreMag, 1},
            {"declination", required_argument, 0, SETTING_DECLINATION},
            {"declination-dec", required_argument, 0, SETTING_DECLINATION_DECIMAL},
            {"prefix", required_argument, 0, SETTING_PREFIX},
            {"index", required_argument, 0, SETTING_INDEX},
            {"unit-gps-speed", required_argument, 0, SETTING_UNIT_GPS_SPEED},
            {"unit-vbat", required_argument, 0, SETTING_UNIT_VBAT},
            {"unit-amperage", required_argument, 0, SETTING_UNIT_AMPERAGE},
            {0, 0, 0, 0}
        };

        int option_index = 0;

        opterr = 0;

        c = getopt_long (argc, argv, "", long_options, &option_index);

        if (c == -1)
            break;

        switch (c) {
            case SETTING_INDEX:
                options.logNumber = atoi(optarg);
            break;
            case SETTING_PREFIX:
                options.outputPrefix = optarg;
            break;
            case SETTING_UNIT_GPS_SPEED:
                if (!parseUnit(optarg, &options.unitGPSSpeed)) {
                    fprintf(stderr, "Bad GPS speed unit\n");
                    exit(-1);
                }
            break;
            case SETTING_UNIT_VBAT:
                if (!parseUnit(optarg, &options.unitVbat)) {
                    fprintf(stderr, "Bad VBAT unit\n");
                    exit(-1);
                }
            break;
            case SETTING_UNIT_AMPERAGE:
                if (!parseUnit(optarg, &options.unitAmperage)) {
                    fprintf(stderr, "Bad Amperage unit\n");
                    exit(-1);
                }
            break;
            case SETTING_DECLINATION:
                imuSetMagneticDeclination(parseDegreesMinutes(optarg));
            break;
            case SETTING_DECLINATION_DECIMAL:
                imuSetMagneticDeclination(atof(optarg));
            break;
            case '\0':
                //Longopt which has set a flag
            break;
            case ':':
                fprintf(stderr, "%s: option '%s' requires an argument\n", argv[0], argv[optind-1]);
                exit(-1);
            break;
            default:
                if (optopt == 0)
                    fprintf(stderr, "%s: option '%s' is invalid\n", argv[0], argv[optind-1]);
                else
                    fprintf(stderr, "%s: option '-%c' is invalid\n", argv[0], optopt);

                exit(-1);
            break;
        }
    }
}

int main(int argc, char **argv)
{
    flightLog_t *log;
    int fd;
    int logIndex;

    parseCommandlineOptions(argc, argv);

    if (options.help || argc == 1) {
        printUsage(argv[0]);
        return -1;
    }

    if (options.toStdout && argc - optind > 1) {
        fprintf(stderr, "You can only decode one log at a time if you're printing to stdout\n");
        return -1;
    }

    for (int i = optind; i < argc; i++) {
        const char *filename = argv[i];

        fd = open(filename, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "Failed to open log file '%s': %s\n\n", filename, strerror(errno));
            continue;
        }

        log = flightLogCreate(fd);

        if (!log) {
            fprintf(stderr, "Failed to read log file '%s'\n\n", filename);
            continue;
        }

        if (log->logCount == 0) {
            fprintf(stderr, "Couldn't find the header of a flight log in the file '%s', is this the right kind of file?\n\n", filename);
            continue;
        }

        if (options.logNumber > 0 || options.toStdout) {
            logIndex = validateLogIndex(log);

            if (logIndex == -1)
                return -1;

            decodeFlightLog(log, filename, logIndex);
        } else {
            //Decode all the logs
            for (logIndex = 0; logIndex < log->logCount; logIndex++)
                decodeFlightLog(log, filename, logIndex);
        }

        flightLogDestroy(log);
    }

    return 0;
}
