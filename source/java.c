#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

/*
 * JNI Methods
*/

// Methods the He-Man .so calls on its Java side (HeloActivity, Chillingo
// TermsNDKWrapper, IAB, analytics, social). Anything not needing a non-zero
// result is registered only so GetMethodID succeeds; FalsoJNI returns 0 for
// ids missing from the typed tables below.
#include <string.h>
#include <stdlib.h>

#define M(id, name, type) { id, name, type }

enum {
	// HeloActivity / class loading
	GET_CLASS_LOADER = 1, LOAD_CLASS, GET_ASSET_ROOT_PATH, GET_VERSION_NAME,
	GET_BUILD_TYPE, GET_VERSION_NUMBER, NEED_TO_AUTHENTICATE, NEED_TO_DOWNLOAD_ASSETS,
	INSTALLER_EXISTS, GET_SCREEN_DENSITY, GET_APPLICATION_LABEL,
	START_AUTHENTICATION, GET_AUTHENTICATION_STATUS, NEED_TO_DRAW_DOWNLOAD_PROGRESS,
	START_DOWNLOAD, GET_DOWNLOAD_PROGRESS, GET_DOWNLOAD_STATUS,
	HAS_NOTIFICATION, GET_LASTEST_NOTIFICATION_DATA_PARAM, SCHEDULE_NOTIFICATION,
	CANCEL_LOCAL_NOTIFICATION, EXTRACT_NEXT_ZIP_FILE_ENTRY, SET_ASSET_STRING,
	GET_ZIP_EXTRACTION_STEPS,
	// Analytics
	START_SESSION, STOP_SESSION, LOG_EVENT, LOG_MONETIZATION, SEND_DEVICE_INFO,
	// Chillingo terms
	TERMS_INIT, TERMS_INIT_ANON, INIT_TERMS, RESET_CALLBACK_BOOLEANS, DISPLAY_DIALOG,
	DISPLAY_EULA, DISPLAY_PRIVACY_POLICY, DISPLAY_TOS, AGE_NOT_MET, AGE_PENDING,
	AGE_MET, IS_INITIALIZED, IS_DIALOG_STILL_DISPLAYED,
	// Social
	REVIEW_GAME, CAN_REVIEW_GAME, CONNECT_TO_URL, POST_FEED, IS_LOGGED_IN,
	GET_LOGIN_RESPONSE, LOGOUT, LOGIN, FEED,
	// In-app billing
	INIT_IAB_CONNECTION, RESTORE_TRANSACTIONS, PURCHASE_PRODUCT,
	GET_PROVISION_PRODUCT_STATUS, GET_PRODUCT_DESCRIPTION, GET_PURCHASED_ITEMS_TO_SYNCH,
	GET_PRODUCT_RESTORE_STATUS, GET_PURCHASE_STATUS, REQUEST_PRODUCT_INFO,
	GET_PRODUCT_TITLE, CONSUME_PRODUCT, GET_PRODUCT_PRICE, GET_PRODUCT_INFO_RETRIEVAL_STATUS,
};

NameToMethodID nameToMethodId[] = {
	M(GET_CLASS_LOADER, "getClassLoader", METHOD_TYPE_OBJECT),
	M(LOAD_CLASS, "loadClass", METHOD_TYPE_OBJECT),
	M(GET_ASSET_ROOT_PATH, "getAssetRootPath", METHOD_TYPE_OBJECT),
	M(GET_VERSION_NAME, "getVersionName", METHOD_TYPE_OBJECT),
	M(GET_BUILD_TYPE, "getBuildType", METHOD_TYPE_INT),
	M(GET_VERSION_NUMBER, "getVersionNumber", METHOD_TYPE_INT),
	M(NEED_TO_AUTHENTICATE, "needToAuthenticate", METHOD_TYPE_BOOLEAN),
	M(NEED_TO_DOWNLOAD_ASSETS, "needToDownloadAssets", METHOD_TYPE_BOOLEAN),
	M(INSTALLER_EXISTS, "installerExists", METHOD_TYPE_BOOLEAN),
	M(GET_SCREEN_DENSITY, "getScreenDensity", METHOD_TYPE_FLOAT),
	M(GET_APPLICATION_LABEL, "getApplicationLabel", METHOD_TYPE_OBJECT),
	M(START_AUTHENTICATION, "startAuthentication", METHOD_TYPE_VOID),
	M(GET_AUTHENTICATION_STATUS, "getAuthenticationStatus", METHOD_TYPE_INT),
	M(NEED_TO_DRAW_DOWNLOAD_PROGRESS, "needToDrawDownloadProgress", METHOD_TYPE_BOOLEAN),
	M(START_DOWNLOAD, "startDownload", METHOD_TYPE_VOID),
	M(GET_DOWNLOAD_PROGRESS, "getDownloadProgress", METHOD_TYPE_INT),
	M(GET_DOWNLOAD_STATUS, "getDownloadStatus", METHOD_TYPE_INT),
	M(HAS_NOTIFICATION, "hasNotification", METHOD_TYPE_BOOLEAN),
	M(GET_LASTEST_NOTIFICATION_DATA_PARAM, "getLastestNotificationDataParam", METHOD_TYPE_INT),
	M(SCHEDULE_NOTIFICATION, "scheduleNotification", METHOD_TYPE_VOID),
	M(CANCEL_LOCAL_NOTIFICATION, "cancelLocalNotification", METHOD_TYPE_VOID),
	M(EXTRACT_NEXT_ZIP_FILE_ENTRY, "extractNextZipFileEntry", METHOD_TYPE_INT),
	M(SET_ASSET_STRING, "setAssetString", METHOD_TYPE_VOID),
	M(GET_ZIP_EXTRACTION_STEPS, "getZipExtractionSteps", METHOD_TYPE_INT),

	M(START_SESSION, "startSession", METHOD_TYPE_VOID),
	M(STOP_SESSION, "stopSession", METHOD_TYPE_VOID),
	M(LOG_EVENT, "logEvent", METHOD_TYPE_VOID),
	M(LOG_MONETIZATION, "logMonetization", METHOD_TYPE_VOID),
	M(SEND_DEVICE_INFO, "sendDeviceInfo", METHOD_TYPE_VOID),

	// Constructors are looked up as "<class>/<init>"
	M(TERMS_INIT, "com/chillingo/libterms/TermsNDKWrapper/<init>", METHOD_TYPE_OBJECT),
	M(TERMS_INIT_ANON, "/<init>", METHOD_TYPE_OBJECT),
	M(INIT_TERMS, "initTerms", METHOD_TYPE_BOOLEAN),
	M(RESET_CALLBACK_BOOLEANS, "resetCallbackBooleans", METHOD_TYPE_VOID),
	M(DISPLAY_DIALOG, "displayDialog", METHOD_TYPE_VOID),
	M(DISPLAY_EULA, "displayEndUserLicenseAgreement", METHOD_TYPE_VOID),
	M(DISPLAY_PRIVACY_POLICY, "displayPrivacyPolicy", METHOD_TYPE_VOID),
	M(DISPLAY_TOS, "displayTermsOfService", METHOD_TYPE_VOID),
	M(AGE_NOT_MET, "ageVerificationCriteriaNotMetCallbackCalled", METHOD_TYPE_BOOLEAN),
	M(AGE_PENDING, "ageVerificationPendingDialogDisplayCallbackCalled", METHOD_TYPE_BOOLEAN),
	M(AGE_MET, "ageVerificationCriteriaMetCallbackCalled", METHOD_TYPE_BOOLEAN),
	M(IS_INITIALIZED, "isInitialized", METHOD_TYPE_BOOLEAN),
	M(IS_DIALOG_STILL_DISPLAYED, "isDialogStillDisplayed", METHOD_TYPE_BOOLEAN),

	M(REVIEW_GAME, "reviewGame", METHOD_TYPE_VOID),
	M(CAN_REVIEW_GAME, "canReviewGame", METHOD_TYPE_BOOLEAN),
	M(CONNECT_TO_URL, "connectToURL", METHOD_TYPE_BOOLEAN),
	M(POST_FEED, "postFeed", METHOD_TYPE_VOID),
	M(IS_LOGGED_IN, "isLoggedIn", METHOD_TYPE_BOOLEAN),
	M(GET_LOGIN_RESPONSE, "getLoginResponse", METHOD_TYPE_INT),
	M(LOGOUT, "logout", METHOD_TYPE_VOID),
	M(LOGIN, "login", METHOD_TYPE_VOID),
	M(FEED, "feed", METHOD_TYPE_BOOLEAN),

	M(INIT_IAB_CONNECTION, "initIabConnection", METHOD_TYPE_VOID),
	M(RESTORE_TRANSACTIONS, "restoreTransactions", METHOD_TYPE_VOID),
	M(PURCHASE_PRODUCT, "purchaseProduct", METHOD_TYPE_BOOLEAN),
	M(GET_PROVISION_PRODUCT_STATUS, "getProvisionProductStatus", METHOD_TYPE_INT),
	M(GET_PRODUCT_DESCRIPTION, "getProductDescription", METHOD_TYPE_OBJECT),
	M(GET_PURCHASED_ITEMS_TO_SYNCH, "getPurchasedItemsToSynch", METHOD_TYPE_OBJECT),
	M(GET_PRODUCT_RESTORE_STATUS, "getProductRestoreStatus", METHOD_TYPE_INT),
	M(GET_PURCHASE_STATUS, "getPurchaseStatus", METHOD_TYPE_INT),
	M(REQUEST_PRODUCT_INFO, "requestProductInfo", METHOD_TYPE_BOOLEAN),
	M(GET_PRODUCT_TITLE, "getProductTitle", METHOD_TYPE_OBJECT),
	M(CONSUME_PRODUCT, "consumeProduct", METHOD_TYPE_BOOLEAN),
	M(GET_PRODUCT_PRICE, "getProductPrice", METHOD_TYPE_OBJECT),
	M(GET_PRODUCT_INFO_RETRIEVAL_STATUS, "getProductInfoRetrievalStatus", METHOD_TYPE_INT),
};

static jstring jstr(const char *s) { return (*(&jni))->NewStringUTF(&jni, s); }

// FalsoJNI's reserved fake object id: never dereferenced or freed by FalsoJNI
#define FAKE_OBJECT ((jobject)0x42424242)

jobject getClassLoader(jmethodID id, va_list args) { return FAKE_OBJECT; }

// FalsoJNI treats a jclass as a C string with the class name (it builds
// constructor ids as "<class>/<init>"), so return the requested name with
// Java dots converted to JNI slashes.
jobject loadClass(jmethodID id, va_list args) {
	jstring name = va_arg(args, jstring);
	const char *utf = name ? (*(&jni))->GetStringUTFChars(&jni, name, NULL) : NULL;
	if (!utf)
		return (jobject)"";
	char *cls = strdup(utf);
	(*(&jni))->ReleaseStringUTFChars(&jni, name, utf);
	for (char *c = cls; *c; c++)
		if (*c == '.') *c = '/';
	return (jobject)cls;
}

// The game builds paths as <root> + "/SaveData/..." and <root> + "/screen-xlarge/..."
jobject getAssetRootPath(jmethodID id, va_list args) { return (jobject)jstr("ux0:data/heman"); }
jobject getVersionName(jmethodID id, va_list args) { return (jobject)jstr("1.0"); }
jobject getApplicationLabel(jmethodID id, va_list args) { return (jobject)jstr("He-Man"); }
jobject emptyString(jmethodID id, va_list args) { return (jobject)jstr(""); }
jobject newDummyObject(jmethodID id, va_list args) { return FAKE_OBJECT; }

jint getVersionNumber(jmethodID id, va_list args) { return 1; }
// The game reads getVersionNumber through CallFloatMethod
jfloat getVersionNumberF(jmethodID id, va_list args) { return 1.0f; }
jfloat getScreenDensity(jmethodID id, va_list args) { return 1.0f; }

jboolean jtrue(jmethodID id, va_list args) { return JNI_TRUE; }

MethodsBoolean methodsBoolean[] = {
	{ INIT_TERMS,     jtrue },
	{ IS_INITIALIZED, jtrue },
	{ AGE_MET,        jtrue },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {
	{ GET_VERSION_NUMBER, getVersionNumberF },
	{ GET_SCREEN_DENSITY, getScreenDensity },
};
MethodsInt methodsInt[] = {
	{ GET_VERSION_NUMBER, getVersionNumber },
};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {
	{ GET_CLASS_LOADER,        getClassLoader },
	{ LOAD_CLASS,              loadClass },
	{ GET_ASSET_ROOT_PATH,     getAssetRootPath },
	{ GET_VERSION_NAME,        getVersionName },
	{ GET_APPLICATION_LABEL,   getApplicationLabel },
	{ GET_PRODUCT_DESCRIPTION, emptyString },
	{ GET_PRODUCT_TITLE,       emptyString },
	{ GET_PRODUCT_PRICE,       emptyString },
	{ TERMS_INIT,              newDummyObject },
	{ TERMS_INIT_ANON,         newDummyObject },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {};

/*
 * JNI Fields
*/

// System-wide constant that applications sometimes request
// https://developer.android.com/reference/android/content/Context.html#WINDOW_SERVICE
char WINDOW_SERVICE[] = "window";

// System-wide constant that's often used to determine Android version
// https://developer.android.com/reference/android/os/Build.VERSION.html#SDK_INT
// Possible values: https://developer.android.com/reference/android/os/Build.VERSION_CODES
const int SDK_INT = 19; // Android 4.4 / KitKat

NameToFieldID nameToFieldId[] = {
		{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT }, 
		{ 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
		{ 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
		{ 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
