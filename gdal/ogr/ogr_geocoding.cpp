/******************************************************************************
 * $Id$
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Client of geocoding service.
 * Author:   Even Rouault, <even dot rouault at mines dash paris dot org>
 *
 ******************************************************************************
 * Copyright (c) 2012, Even Rouault
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include <sys/time.h>

#include "cpl_conv.h"
#include "cpl_http.h"
#include "cpl_multiproc.h"
#include "cpl_minixml.h"

#include "ogr_geocoding.h"
#include "ogr_mem.h"
#include "ogrsf_frmts.h"

CPL_CVSID("$Id$");

struct _OGRGeocodingSessionHS
{
    char*  pszCacheFilename;
    char*  pszGeocodingService;
    char*  pszEmail;
    char*  pszApplication;
    char*  pszQueryTemplate;
    char*  pszExtraQueryParameters;
    int    bReadCache;
    int    bWriteCache;
    double dfDelayBetweenQueries;
    OGRDataSource* poDS;
};

static void* hMutex = NULL;
static double dfLastQueryTimeStampOSMNominatim = 0.0;
static double dfLastQueryTimeStampMapQuestNominatim = 0.0;

#define OSM_NOMINATIM_QUERY      "http://nominatim.openstreetmap.org/search?q=%s&format=xml&polygon_text=1&addressdetails=1"
#define MAPQUEST_NOMINATIM_QUERY "http://open.mapquestapi.com/nominatim/v1/search.php?q=%s&format=xml&addressdetails=1"

#define CACHE_LAYER_NAME         "ogr_geocode_cache"
#define DEFAULT_CACHE_SQLITE     "ogr_geocode_cache.sqlite"
#define DEFAULT_CACHE_CSV        "ogr_geocode_cache.csv"

#define FIELD_URL                "url"
#define FIELD_BLOB               "blob"

/************************************************************************/
/*                       OGRGeocodeGetParameter()                       */
/************************************************************************/

static
const char* OGRGeocodeGetParameter(char** papszOptions, const char* pszKey,
                                   const char* pszDefaultValue)
{
    const char* pszRet = CSLFetchNameValue(papszOptions, pszKey);
    if( pszRet != NULL )
        return pszRet;

    return CPLGetConfigOption(CPLSPrintf("OGR_GEOCODE_%s", pszKey),
                              pszDefaultValue);
}

/************************************************************************/
/*                      OGRGeocodeHasStringValidFormat()                */
/************************************************************************/

/* Checks that pszQueryTemplate has one and only one occurence of %s in it. */
static
int OGRGeocodeHasStringValidFormat(const char* pszQueryTemplate)
{
    const char* pszIter = pszQueryTemplate;
    int bValidFormat = TRUE;
    int bFoundPctS = FALSE;
    while( *pszIter != '\0' )
    {
        if( *pszIter == '%' )
        {
            if( pszIter[1] == '%' )
            {
                pszIter ++;
            }
            else if( pszIter[1] == 's' )
            {
                if( bFoundPctS )
                {
                    bValidFormat = FALSE;
                    break;
                }
                bFoundPctS = TRUE;
            }
            else
            {
                bValidFormat = FALSE;
                break;
            }
        }
        pszIter ++;
    }
    if( !bFoundPctS )
        bValidFormat = FALSE;
    return bValidFormat;
}

/************************************************************************/
/*                       OGRGeocodeCreateSession()                      */
/************************************************************************/

/**
 * \brief Creates a session handle for geocoding requests.
 *
 * Available papszOptions values:
 * <ul>
 * <li> "CACHE_FILE" : Defaults to "ogr_geocode_cache.sqlite" (or otherwise
 *                    "ogr_geocode_cache.csv" if the SQLite driver isn't
 *                    available). Might be any CSV, SQLite or PostgreSQL
 *                    datasource.
 * <li> "READ_CACHE" : "TRUE" (default) or "FALSE"
 * <li> "WRITE_CACHE" : "TRUE" (default) or "FALSE"
 * <li> "SERVICE": "OSM_NOMINATIM" (default), "MAPQUEST_NOMINATIM", or
 *       other value
 * <li> "EMAIL": used by OSM_NOMINATIM. Optional, but recommanded.
 * <li> "APPLICATION": used to set the User-Agent MIME header. Defaults
 *       to GDAL/OGR version string.
 * <li> "DELAY": minimum delay, in second, between 2 consecutive queries.
 *       Defaults to 1.0.
 * <li> "QUERY_TEMPLATE": URL template for GET requests. Must contain one
 *       and only one occurence of %s in it. If not specified, for
 *       SERVICE=OSM_NOMINATIM or SERVICE=MAPQUEST_NOMINATIM, the URL template
 *       is hard-coded.
 * <li> "EXTRA_QUERY_PARAMETERS": additionnal parameters for the GET request.
 * </ul>
 *
 * All the above options can also be set by defining the configuration option
 * of the same name, prefixed by OGR_GEOCODE_. For example "OGR_GEOCODE_SERVICE"
 * for the "SERVICE" option.
 *
 * @param papszOptions NULL, or a NULL-terminated list of string options.
 *
 * @return an handle that should be freed with OGRGeocodeDestroySession(), or NULL
 *         in case of failure.
 *
 * @since GDAL 1.10
 */

OGRGeocodingSessionH OGRGeocodeCreateSession(char** papszOptions)
{
    OGRGeocodingSessionH hSession =
        (OGRGeocodingSessionH)CPLCalloc(1, sizeof(_OGRGeocodingSessionHS));

    const char* pszCacheFilename = OGRGeocodeGetParameter(papszOptions,
                                                          "CACHE_FILE",
                                                          DEFAULT_CACHE_SQLITE);
    CPLString osExt = CPLGetExtension(pszCacheFilename);
    if( !(EQUALN(pszCacheFilename, "PG:", 3) ||
        EQUAL(osExt, "csv") || EQUAL(osExt, "sqlite")) )
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Only .csv, .sqlite or PG: datasources are handled for now.");
        OGRGeocodeDestroySession(hSession);
        return NULL;
    }
    hSession->pszCacheFilename = CPLStrdup(pszCacheFilename);

    hSession->bReadCache = CSLTestBoolean(
        OGRGeocodeGetParameter(papszOptions, "READ_CACHE", "TRUE"));
    hSession->bWriteCache = CSLTestBoolean(
        OGRGeocodeGetParameter(papszOptions, "WRITE_CACHE", "TRUE"));

    const char* pszGeocodingService = OGRGeocodeGetParameter(papszOptions,
                                                             "SERVICE",
                                                             "OSM_NOMINATIM");
    hSession->pszGeocodingService = CPLStrdup(pszGeocodingService);

    const char* pszEmail = OGRGeocodeGetParameter(papszOptions, "EMAIL", NULL);
    hSession->pszEmail = pszEmail ? CPLStrdup(pszEmail) : NULL;

    const char* pszApplication = OGRGeocodeGetParameter(papszOptions,
                                                        "APPLICATION",
                                                        GDALVersionInfo(""));
    hSession->pszApplication = CPLStrdup(pszApplication);

    const char* pszDelayBetweenQueries = OGRGeocodeGetParameter(papszOptions,
                                                                "DELAY", "1.0");
    hSession->dfDelayBetweenQueries = CPLAtofM(pszDelayBetweenQueries);

    const char* pszQueryTemplateDefault = NULL;
    if( EQUAL(pszGeocodingService, "OSM_NOMINATIM") )
        pszQueryTemplateDefault = OSM_NOMINATIM_QUERY;
    else if( EQUAL(pszGeocodingService, "MAPQUEST_NOMINATIM") )
        pszQueryTemplateDefault = MAPQUEST_NOMINATIM_QUERY;
    const char* pszQueryTemplate = OGRGeocodeGetParameter(papszOptions,
                                                          "QUERY_TEMPLATE",
                                                          pszQueryTemplateDefault);
    if( pszQueryTemplate == NULL )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "QUERY_TEMPLATE parameter not defined");
        OGRGeocodeDestroySession(hSession);
        return NULL;
    }

    if( !OGRGeocodeHasStringValidFormat(pszQueryTemplate) )
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "QUERY_TEMPLATE value has an invalid format");
        OGRGeocodeDestroySession(hSession);
        return NULL;
    }

    hSession->pszQueryTemplate = CPLStrdup(pszQueryTemplate);

    const char* pszExtraQueryParameters = OGRGeocodeGetParameter(
                                papszOptions, "EXTRA_QUERY_PARAMETERS", NULL);
    hSession->pszExtraQueryParameters = pszExtraQueryParameters ?
                                        CPLStrdup(pszExtraQueryParameters) : NULL;

    return hSession;
}

/************************************************************************/
/*                       OGRGeocodeDestroySession()                     */
/************************************************************************/

/**
 * \brief Destroys a session handle for geocoding requests.

 * @param hSession the handle to destroy.
 *
 * @since GDAL 1.10
 */
void OGRGeocodeDestroySession(OGRGeocodingSessionH hSession)
{
    if( hSession == NULL )
        return;
    CPLFree(hSession->pszCacheFilename);
    CPLFree(hSession->pszGeocodingService);
    CPLFree(hSession->pszEmail);
    CPLFree(hSession->pszApplication);
    CPLFree(hSession->pszQueryTemplate);
    CPLFree(hSession->pszExtraQueryParameters);
    if( hSession->poDS )
        OGRReleaseDataSource((OGRDataSourceH) hSession->poDS);
    CPLFree(hSession);
}

/************************************************************************/
/*                        OGRGeocodeGetCacheLayer()                     */
/************************************************************************/

static OGRLayer* OGRGeocodeGetCacheLayer(OGRGeocodingSessionH hSession,
                                         int bCreateIfNecessary,
                                         int* pnIdxBlob)
{
    OGRDataSource* poDS = hSession->poDS;
    CPLString osExt = CPLGetExtension(hSession->pszCacheFilename);

    if( poDS == NULL )
    {
        if( OGRGetDriverCount() == 0 )
            OGRRegisterAll();

        char* pszOldVal = CPLGetConfigOption("OGR_SQLITE_SYNCHRONOUS", NULL) ?
            CPLStrdup(CPLGetConfigOption("OGR_SQLITE_SYNCHRONOUS", NULL)) : NULL;
        CPLSetThreadLocalConfigOption("OGR_SQLITE_SYNCHRONOUS", "OFF");

        poDS = (OGRDataSource*) OGROpen(hSession->pszCacheFilename, TRUE, NULL);
        if( poDS == NULL &&
            EQUAL(hSession->pszCacheFilename, DEFAULT_CACHE_SQLITE) )
        {
            poDS = (OGRDataSource*) OGROpen(DEFAULT_CACHE_CSV, TRUE, NULL);
            if( poDS != NULL )
            {
                CPLFree(hSession->pszCacheFilename);
                hSession->pszCacheFilename = CPLStrdup(DEFAULT_CACHE_CSV);
                CPLDebug("OGR", "Switch geocode cache file to %s",
                         hSession->pszCacheFilename);
                osExt = "csv";
            }
        }

        if( bCreateIfNecessary && poDS == NULL &&
            !EQUALN(hSession->pszCacheFilename, "PG:", 3) )
        {
            OGRSFDriverH hDriver = OGRGetDriverByName(osExt);
            if( hDriver == NULL &&
                EQUAL(hSession->pszCacheFilename, DEFAULT_CACHE_SQLITE) )
            {
                CPLFree(hSession->pszCacheFilename);
                hSession->pszCacheFilename = CPLStrdup(DEFAULT_CACHE_CSV);
                CPLDebug("OGR", "Switch geocode cache file to %s",
                         hSession->pszCacheFilename);
                osExt = "csv";
                hDriver = OGRGetDriverByName(osExt);
            }
            if( hDriver != NULL )
            {
                char** papszOptions = NULL;
                if( EQUAL(osExt, "SQLITE") )
                {
                    papszOptions = CSLAddNameValue(papszOptions,
                                                   "METADATA", "FALSE");
                }

                poDS = (OGRDataSource*) OGR_Dr_CreateDataSource(
                            hDriver, hSession->pszCacheFilename, papszOptions);

                if( poDS == NULL &&
                    (EQUAL(osExt, "SQLITE") || EQUAL(osExt, "CSV")))
                {
                    CPLFree(hSession->pszCacheFilename);
                    hSession->pszCacheFilename = CPLStrdup(
                        CPLSPrintf("/vsimem/%s.%s",
                                   CACHE_LAYER_NAME, osExt.c_str()));
                    CPLDebug("OGR", "Switch geocode cache file to %s",
                         hSession->pszCacheFilename);
                    poDS = (OGRDataSource*) OGR_Dr_CreateDataSource(
                            hDriver, hSession->pszCacheFilename, papszOptions);
                }

                CSLDestroy(papszOptions);
            }
        }

        CPLSetThreadLocalConfigOption("OGR_SQLITE_SYNCHRONOUS", pszOldVal);

        if( poDS == NULL )
            return NULL;

        hSession->poDS = poDS;
    }

    CPLPushErrorHandler(CPLQuietErrorHandler);
    OGRLayer* poLayer = poDS->GetLayerByName(CACHE_LAYER_NAME);
    CPLPopErrorHandler();

    if( bCreateIfNecessary && poLayer == NULL )
    {
        poLayer = poDS->CreateLayer(
                        CACHE_LAYER_NAME, NULL, wkbNone, NULL);
        if( poLayer != NULL )
        {
            OGRFieldDefn oFieldDefnURL(FIELD_URL, OFTString);
            poLayer->CreateField(&oFieldDefnURL);
            OGRFieldDefn oFieldDefnBlob(FIELD_BLOB, OFTString);
            poLayer->CreateField(&oFieldDefnBlob);
            if( EQUAL(osExt, "SQLITE") ||
                EQUALN(hSession->pszCacheFilename, "PG:", 3) )
            {
                const char* pszSQL =
                    CPLSPrintf( "CREATE INDEX idx_%s_%s ON %s(%s)",
                                FIELD_URL, poLayer->GetName(),
                                poLayer->GetName(), FIELD_URL );
                poDS->ExecuteSQL(pszSQL, NULL, NULL);
            }
        }
    }

    int nIdxBlob = -1;
    if( poLayer == NULL ||
        poLayer->GetLayerDefn()->GetFieldIndex(FIELD_URL) < 0 ||
        (nIdxBlob = poLayer->GetLayerDefn()->GetFieldIndex(FIELD_BLOB)) < 0 )
    {
        return NULL;
    }

    if( pnIdxBlob )
        *pnIdxBlob = nIdxBlob;

    return poLayer;
}

/************************************************************************/
/*                        OGRGeocodeGetFromCache()                      */
/************************************************************************/

static char* OGRGeocodeGetFromCache(OGRGeocodingSessionH hSession,
                                    const char* pszURL)
{
    CPLMutexHolderD(&hMutex);

    int nIdxBlob = -1;
    OGRLayer* poLayer = OGRGeocodeGetCacheLayer(hSession, FALSE, &nIdxBlob);
    if( poLayer == NULL )
        return NULL;

    char* pszSQLEscapedURL = CPLEscapeString(pszURL, -1, CPLES_SQL);
    poLayer->SetAttributeFilter(CPLSPrintf("%s='%s'", FIELD_URL, pszSQLEscapedURL));
    CPLFree(pszSQLEscapedURL);

    char* pszRet = NULL;
    OGRFeature* poFeature = poLayer->GetNextFeature();
    if( poFeature != NULL )
    {
        if( poFeature->IsFieldSet(nIdxBlob) )
            pszRet = CPLStrdup(poFeature->GetFieldAsString(nIdxBlob));
        OGRFeature::DestroyFeature(poFeature);
    }

    return pszRet;
}

/************************************************************************/
/*                        OGRGeocodePutIntoCache()                      */
/************************************************************************/

static int OGRGeocodePutIntoCache(OGRGeocodingSessionH hSession,
                                  const char* pszURL,
                                  const char* pszContent)
{
    CPLMutexHolderD(&hMutex);

    int nIdxBlob = -1;
    OGRLayer* poLayer = OGRGeocodeGetCacheLayer(hSession, TRUE, &nIdxBlob);
    if( poLayer == NULL )
        return FALSE;

    OGRFeature* poFeature = new OGRFeature(poLayer->GetLayerDefn());
    poFeature->SetField(FIELD_URL, pszURL);
    poFeature->SetField(FIELD_BLOB, pszContent);
    int bRet = poLayer->CreateFeature(poFeature) == OGRERR_NONE;
    delete poFeature;

    return bRet;
}

/************************************************************************/
/*                         OGRGeocodeBuildLayer()                       */
/************************************************************************/

static OGRLayerH OGRGeocodeBuildLayer(const char* pszContent)
{
    CPLXMLNode* psRoot = CPLParseXMLString( pszContent );
    if( psRoot == NULL )
        return NULL;
    CPLXMLNode* psSearchResults = CPLSearchXMLNode(psRoot, "=searchresults");
    if( psSearchResults == NULL )
    {
        CPLDestroyXMLNode( psRoot );
        return NULL;
    }
    OGRMemLayer* poLayer = new OGRMemLayer( "place", NULL, wkbUnknown );
    OGRFeatureDefn* poFDefn = poLayer->GetLayerDefn();

    CPLXMLNode* psPlace = psSearchResults->psChild;
    while( psPlace != NULL )
    {
        if( psPlace->eType == CXT_Element &&
            strcmp(psPlace->pszValue, "place") == 0 )
        {
            int bFoundLat = FALSE, bFoundLon = FALSE;
            double dfLat = 0.0, dfLon = 0.0;

            /* First iteration to add fields */
            CPLXMLNode* psChild = psPlace->psChild;
            while( psChild != NULL )
            {
                const char* pszName = psChild->pszValue;
                const char* pszVal = CPLGetXMLValue(psChild, NULL, NULL);
                if( poFDefn->GetFieldIndex(pszName) < 0 &&
                    strcmp(pszName, "geotext") != 0 )
                {
                    OGRFieldDefn oFieldDefn(pszName, OFTString);
                    if( strcmp(pszName, "place_rank") == 0 )
                    {
                        oFieldDefn.SetType(OFTInteger);
                    }
                    else if( strcmp(pszName, "lat") == 0 )
                    {
                        if( pszVal != NULL )
                        {
                            bFoundLat = TRUE;
                            dfLat = CPLAtofM(pszVal);
                        }
                        oFieldDefn.SetType(OFTReal);
                    }
                    else if( strcmp(pszName, "lon") == 0 )
                    {
                        if( pszVal != NULL )
                        {
                            bFoundLon = TRUE;
                            dfLon = CPLAtofM(pszVal);
                        }
                        oFieldDefn.SetType(OFTReal);
                    }
                    poLayer->CreateField(&oFieldDefn);
                }
                psChild = psChild->psNext;
            }

            /* Second iteration to fill the feature */
            OGRFeature* poFeature = new OGRFeature(poFDefn);
            psChild = psPlace->psChild;
            while( psChild != NULL )
            {
                int nIdx;
                const char* pszName = psChild->pszValue;
                const char* pszVal = CPLGetXMLValue(psChild, NULL, NULL);
                if( (nIdx = poFDefn->GetFieldIndex(pszName)) >= 0 )
                {
                    if( pszVal != NULL )
                        poFeature->SetField(nIdx, pszVal);
                }
                else if( strcmp(pszName, "geotext") == 0 )
                {
                    char* pszWKT = (char*) pszVal;
                    if( pszWKT != NULL )
                    {
                        OGRGeometry* poGeometry = NULL;
                        OGRGeometryFactory::createFromWkt(&pszWKT, NULL,
                                                          &poGeometry);
                        if( poGeometry )
                            poFeature->SetGeometryDirectly(poGeometry);
                    }
                }
                psChild = psChild->psNext;
            }

            /* If we didn't found an explicit geometry, build it from */
            /* the 'lon' and 'lat' attributes. */
            if( poFeature->GetGeometryRef() == NULL && bFoundLon && bFoundLat )
                poFeature->SetGeometryDirectly(new OGRPoint(dfLon, dfLat));

            poLayer->CreateFeature(poFeature);
            delete poFeature;
        }
        psPlace = psPlace->psNext;
    }

    CPLDestroyXMLNode( psRoot );
    return (OGRLayerH) poLayer;
}

/************************************************************************/
/*                              OGRGeocode()                            */
/************************************************************************/

/**
 * \brief Runs a geocoding request.
 *
 * If the result is not found in cache, a GET request will be sent to resolve
 * the query.
 *
 * Note: most online services have Term of Uses. You are kindly requested
 * to read and follow them. For the OpenStreetMap Nominatim service, this
 * implementation will make sure that no more than one request is sent by
 * second, but there might be other restrictions that you must follow by other
 * means.
 *
 * In case of success, the return of this function is a OGR layer that contain
 * zero, one or several features matching the query. Note that the geometry of the
 * features is not necessarily a point.  The returned layer must be freed with
 * OGRGeocodeFreeResult().
 *
 * Note: this function is also available as the SQL ogr_geocode() function of
 * the SQL SQLite dialect.
 *
 * @param hSession the geocoding session handle.
 * @param pszQuery the string to geocode.
 * @param papszStructuredQuery unused for now. Must be NULL.
 * @param papszOptions unused for now.
 *
 * @return a OGR layer with the result(s), or NULL in case of error.
 *         The returned layer must be freed with OGRGeocodeFreeResult().
 *
 * @since GDAL 1.10
 */
OGRLayerH OGRGeocode(OGRGeocodingSessionH hSession,
                     const char* pszQuery,
                     char** papszStructuredQuery,
                     char** papszOptions)
{
    VALIDATE_POINTER1( hSession, "OGRGeocode", NULL );
    if( (pszQuery == NULL && papszStructuredQuery == NULL) ||
        (pszQuery != NULL && papszStructuredQuery != NULL) )
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Only one of pszQuery or papszStructuredQuery must be set.");
        return NULL;
    }

    if( papszStructuredQuery != NULL )
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "papszStructuredQuery not yet supported.");
        return NULL;
    }

    char* pszEscapedQuery = CPLEscapeString(pszQuery, -1, CPLES_URL);
    CPLString osURL = CPLSPrintf(hSession->pszQueryTemplate, pszEscapedQuery);
    CPLFree(pszEscapedQuery);

    if( hSession->pszExtraQueryParameters != NULL )
    {
        osURL += "&";
        osURL += hSession->pszExtraQueryParameters;
    }

    CPLString osURLWithEmail = osURL;
    if( EQUAL(hSession->pszGeocodingService, "OSM_NOMINATIM") &&
        hSession->pszEmail != NULL )
    {
        char* pszEscapedEmail = CPLEscapeString(hSession->pszEmail,
                                                -1, CPLES_URL);
        osURLWithEmail = osURL + "&email=" + pszEscapedEmail;
        CPLFree(pszEscapedEmail);
    }

    OGRLayerH hLayer = NULL;

    char* pszCachedResult = NULL;
    if( hSession->bReadCache )
        pszCachedResult = OGRGeocodeGetFromCache(hSession, osURL);
    if( pszCachedResult == NULL )
    {
        CPLHTTPResult* psResult;

        double* pdfLastQueryTime = NULL;
        if( EQUAL(hSession->pszGeocodingService, "OSM_NOMINATIM") )
            pdfLastQueryTime = &dfLastQueryTimeStampOSMNominatim;
        else if( EQUAL(hSession->pszGeocodingService, "MAPQUEST_NOMINATIM") )
            pdfLastQueryTime = &dfLastQueryTimeStampMapQuestNominatim;

        char** papszHTTPOptions = NULL;
        papszHTTPOptions = CSLAddNameValue(papszHTTPOptions, "HEADERS",
            CPLSPrintf("User-Agent: %s", hSession->pszApplication));

        if( pdfLastQueryTime != NULL )
        {
            CPLMutexHolderD(&hMutex);
            struct timeval tv;

            gettimeofday(&tv, NULL);
            double dfCurrentTime = tv.tv_sec + tv.tv_usec / 1e6;
            if( dfCurrentTime < *pdfLastQueryTime +
                                    hSession->dfDelayBetweenQueries )
            {
                CPLSleep(*pdfLastQueryTime + hSession->dfDelayBetweenQueries -
                         dfCurrentTime);
            }

            psResult = CPLHTTPFetch( osURLWithEmail,  papszHTTPOptions );

            gettimeofday(&tv, NULL);
            *pdfLastQueryTime = tv.tv_sec + tv.tv_usec / 1e6;
        }
        else
            psResult = CPLHTTPFetch( osURLWithEmail,  papszHTTPOptions );

        CSLDestroy(papszHTTPOptions);
        papszHTTPOptions = NULL;

        if( psResult == NULL )
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Query for '%s' failed", pszQuery);
        }
        else
        {
            const char* pszResult = (const char*) psResult->pabyData;
            if( pszResult != NULL )
            {
                if( hSession->bWriteCache )
                    OGRGeocodePutIntoCache(hSession, osURL, pszResult);
                hLayer = OGRGeocodeBuildLayer(pszResult);
            }
            CPLHTTPDestroyResult(psResult);
        }
    }
    else
    {
        hLayer = OGRGeocodeBuildLayer(pszCachedResult);
        CPLFree(pszCachedResult);
    }

    return hLayer;
}

/************************************************************************/
/*                        OGRGeocodeFreeResult()                        */
/************************************************************************/

/**
 * \brief Destroys the result of a geocoding request.
 *
 * @param hLayer the layer returned by OGRGeocode() to destroy.
 *
 * @since GDAL 1.10
 */
void OGRGeocodeFreeResult(OGRLayerH hLayer)
{
    delete (OGRLayer*) hLayer;
}
