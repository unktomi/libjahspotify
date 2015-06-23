/*
 * Licensed to the Apache Software Foundation (ASF) under one
 *        or more contributor license agreements.  See the NOTICE file
 *        distributed with this work for additional information
 *        regarding copyright ownership.  The ASF licenses this file
 *        to you under the Apache License, Version 2.0 (the
 *        "License"); you may not use this file except in compliance
 *        with the License.  You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *        Unless required by applicable law or agreed to in writing,
 *        software distributed under the License is distributed on an
 *        "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *        KIND, either express or implied.  See the License for the
 *        specific language governing permissions and limitations
 *        under the License.
 */

#include <stdio.h>
#include <libspotify/api.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>

#include "Logging.h"
#include "JNIHelpers.h"
#include "JahSpotify.h"
#include "jahspotify_impl_JahSpotifyImpl.h"
#include "AppKey.h"
#include "Callbacks.h"
#include "ThreadHelpers.h"

#define MAX_LENGTH_FOLDER_NAME 256

/// The global session handle
sp_session *g_sess = NULL;
/// Handle to the curren track
sp_track *g_currenttrack = NULL;
static void track_ended(jboolean forced);

jobject g_connectionListener = NULL;
jobject g_playbackListener = NULL;
jobject g_searchCompleteListener = NULL;
jobject g_mediaLoadedListener = NULL;
extern jclass g_linkClass;
extern jclass g_playlistCLass;

pthread_mutex_t g_spotify_mutex;
/// Synchronization mutex for the main thread
static pthread_mutex_t g_notify_mutex;
/// Synchronization condition variable for the main thread
static pthread_cond_t g_notify_cond;
/// Synchronization variable telling the main thread to process events
static int g_notify_do;
/// Non-zero when a track has ended and a new one has not yet started a new one
static int g_playback_done;
static int g_playback_stopped;
static int g_stop_after_logout = 0;
static int g_stop = 0;

static media *loading = NULL;


void populateJAlbumInstanceFromAlbumBrowse(JNIEnv *env, sp_album *album, sp_albumbrowse *albumBrowse, jobject albumInstance);
void populateJArtistInstanceFromArtistBrowse(JNIEnv *env, sp_artistbrowse *artistBrowse, jobject artist);
jobject createJPlaylistInstance(JNIEnv *env, sp_link* link, const char* name, sp_link* image);
jobject createJArtistInstance(JNIEnv *env, sp_artist *artist, int browse);
jobject createJAlbumInstance(JNIEnv *env, sp_album *album, int browse);
jobject createJTrackInstance(JNIEnv *env, sp_track *track);

void populateJTrackInstance(JNIEnv *env, jobject trackInstance, sp_track *track);
void populateJAlbumInstance(JNIEnv *env, jobject albumInstance, sp_album *album, int browse);
void populateJArtistInstance(JNIEnv *env, jobject artistInstance, sp_artist *artist, int browse);

jobject createJPlaylist(JNIEnv *env, jobject playlistInstance, sp_playlist *playlist);
jobject createJLinkInstance(JNIEnv *env, sp_link *link);
static sp_playlist_callbacks pl_callbacks;

/* --------------------------  PLAYLIST CALLBACKS  ------------------------- */
/**
 * Callback from libspotify, saying that a track has been added to a playlist.
 *
 * @param  pl          The playlist handle
 * @param  tracks      An array of track handles
 * @param  num_tracks  The number of tracks in the \c tracks array
 * @param  position    Where the tracks were inserted
 * @param  userdata    The opaque pointer
 */
static void SP_CALLCONV tracks_added(sp_playlist *pl, sp_track * const *tracks, int num_tracks, int position, void *userdata) {
	log_debug("jahspotify", "tracks_added", "Tracks added: playlist: %s numtracks: %d position: %d", sp_playlist_name(pl), num_tracks, position);
}

/**
 * Callback from libspotify, saying that a track has been removed from a playlist.
 *
 * @param  pl          The playlist handle
 * @param  tracks      An array of track indices
 * @param  num_tracks  The number of tracks in the \c tracks array
 * @param  userdata    The opaque pointer
 */
static void SP_CALLCONV tracks_removed(sp_playlist *pl, const int *tracks, int num_tracks, void *userdata) {
	log_debug("jahspotify", "tracks_removed", "Tracks removed: playlist: %s numtracks: %d", sp_playlist_name(pl), num_tracks);
}

/**
 * Callback from libspotify, telling when tracks have been moved around in a playlist.
 *
 * @param  pl            The playlist handle
 * @param  tracks        An array of track indices
 * @param  num_tracks    The number of tracks in the \c tracks array
 * @param  new_position  To where the tracks were moved
 * @param  userdata      The opaque pointer
 */
static void SP_CALLCONV tracks_moved(sp_playlist *pl, const int *tracks, int num_tracks, int new_position, void *userdata) {
	log_debug("jahspotify", "tracks_moved", "Tracks moved: playlist: %s numtracks: %d", sp_playlist_name(pl), num_tracks);
}

/**
 * Callback from libspotify. Something renamed the playlist.
 *
 * @param  pl            The playlist handle
 * @param  userdata      The opaque pointer
 */
static void SP_CALLCONV playlist_renamed(sp_playlist *pl, void *userdata) {
	log_debug("jahspotify", "playlist_renamed", "Playlist renamed: playlist: %s", sp_playlist_name(pl));
	JNIEnv *env = NULL;
	if (!retrieveEnv((JNIEnv*) &env)) return;

	setObjectStringField(env, (jobject) userdata, "name", sp_playlist_name(pl));
	detachThread();
}

static void SP_CALLCONV playlist_state_changed(sp_playlist *pl, void *userdata) {
	sp_link *link = sp_link_create_from_playlist(pl);
	char *linkName = calloc(1, sizeof(char) * 1024);
	log_debug("jahspotify", "playlist_state_changed", "State changed on playlist: %s", sp_playlist_name(pl));
	if (link) {
          sp_link_as_string(link, linkName, 1024);
          log_debug("jahspotify", "playlist_state_changed", "Playlist state changed: %s link: %s (loaded: %s)", sp_playlist_name(pl), linkName,
                    (sp_playlist_is_loaded(pl) ? "yes" : "no"));
          
          jobject playlist = (jobject) userdata;
          if (sp_playlist_is_loaded(pl)) {
            sp_playlist_remove_callbacks(pl, &pl_callbacks, userdata);
            
            JNIEnv* env = NULL;
            if (!retrieveEnv((JNIEnv*) &env)) return;
            
            createJPlaylist(env, playlist, pl);
            (*env)->DeleteGlobalRef(env, playlist);
            detachThread();
          }
          
          sp_link_release(link);
	}
	if (linkName) free(linkName);
}

static void SP_CALLCONV playlist_update_in_progress(sp_playlist *pl, bool done, void *userdata) {
	log_debug("jahspotify","playlist_update_in_progress","Update in progress: %s (done: %s)", sp_playlist_name(pl), (done ? "yes" : "no"));
}

static void SP_CALLCONV playlist_metadata_updated(sp_playlist *pl, void *userdata) {
	log_debug("jahspotify", "playlist_metadata_updated", "Metadata updated: %s", sp_playlist_name(pl));
	// signalMetadataUpdated(pl);
}

/**
 * The callbacks we are interested in for individual playlists.
 */
static sp_playlist_callbacks pl_callbacks = { .tracks_added = &tracks_added, .tracks_removed = &tracks_removed, .tracks_moved = &tracks_moved,
		.playlist_renamed = &playlist_renamed, .playlist_state_changed = &playlist_state_changed, .playlist_update_in_progress = &playlist_update_in_progress,
		.playlist_metadata_updated = &playlist_metadata_updated, };

/* --------------------  PLAYLIST CONTAINER CALLBACKS  --------------------- */
/**
 * Callback from libspotify, telling us a playlist was added to the playlist container.
 *
 * We add our playlist callbacks to the newly added playlist.
 *
 * @param  pc            The playlist container handle
 * @param  pl            The playlist handle
 * @param  position      Index of the added playlist
 * @param  userdata      The opaque pointer
 */
static void SP_CALLCONV playlist_added(sp_playlistcontainer *pc, sp_playlist *pl, int position, void *userdata) {
	log_debug("jahspotify", "playlist_added", "Playlist added: %s (loaded: %s)", sp_playlist_name(pl), sp_playlist_is_loaded(pl) ? "Yes" : "No");

	JNIEnv *env = NULL;
	if (!retrieveEnv((JNIEnv*) &env)) return;

	// Get container.
	jclass jPc = (*env)->FindClass(env, "jahspotify/media/PlaylistContainer");
	if (jPc == NULL ) {
		log_error("jahspotify", "playlist_added", "Unable to get playlistcontainer class.");
		detachThread();
		return;
	}

	// Get 'Playlist addPlaylist(Long)' method
	jmethodID jMethod = (*env)->GetStaticMethodID(env, jPc, "addPlaylist", "(J)Ljahspotify/media/Playlist;");
	long pTr = (long) pl;
	jobject playlist = (*env)->CallStaticObjectMethod(env, jPc, jMethod, (jlong) pTr);

	// If the playlist is null then it was already added.
	if (playlist != NULL)
		createJPlaylist(env, playlist, pl);

	detachThread();
}

/**
 * Callback from libspotify, telling us a playlist was removed from the playlist container.
 *
 * This is the place to remove our playlist callbacks.
 *
 * @param  pc            The playlist container handle
 * @param  pl            The playlist handle
 * @param  position      Index of the removed playlist
 * @param  userdata      The opaque pointer
 */
static void SP_CALLCONV playlist_removed(sp_playlistcontainer *pc, sp_playlist *pl, int position, void *userdata) {
  JNIEnv* env = NULL;
  if (!retrieveEnv((JNIEnv*) &env)) return;
  pthread_mutex_lock(&g_spotify_mutex);
  sp_playlist_remove_callbacks( pl, &pl_callbacks, NULL );
  
  const char *name = sp_playlist_name(pl);
  log_debug("jahspotify", "playlist_removed", "Playlist removed: %s", name);
  
  
  sp_link *link = sp_link_create_from_playlist(pl);
  char *linkName = malloc(sizeof(char) * 100);
  sp_link_as_string(link, linkName, 100);
  jstring jString = (*env)->NewStringUTF(env, linkName);
  
  jclass jPc = (*env)->FindClass(env, "jahspotify/media/PlaylistContainer");
  if (jPc == NULL ) {
    log_error("jahspotify", "playlist_removed", "Unable to get playlistcontainer class.");
    pthread_mutex_unlock(&g_spotify_mutex);
    return;
  }
  jmethodID jMethod = (*env)->GetStaticMethodID(env, jPc, "removePlaylist", "(Ljava/lang/String;)V");
  (*env)->CallStaticVoidMethod(env, jPc, jMethod, jString);
  
  if (linkName) free(linkName);
  if (jString) (*env)->DeleteLocalRef(env, jString);
  pthread_mutex_unlock(&g_spotify_mutex);
}

/**
 * Callback from libspotify, telling us the rootlist is fully synchronized
 *
 * @param  pc            The playlist container handle
 * @param  userdata      The opaque pointer
 */
static void SP_CALLCONV container_loaded(sp_playlistcontainer *pc, void *userdata) {
  pthread_mutex_lock(&g_spotify_mutex);
  int i;
  // Make sure all playlists are added.
  for (i = 0; i < sp_playlistcontainer_num_playlists(pc); ++i) {
    sp_playlist *pl = sp_playlistcontainer_playlist(pc, i);
    playlist_added(pc, pl, i, userdata);
  }
  signalPlaylistsLoaded();
  pthread_mutex_unlock(&g_spotify_mutex);
 }

/**
 * The playlist container callbacks
 */
static sp_playlistcontainer_callbacks pc_callbacks = { .playlist_added = &playlist_added, .playlist_removed = &playlist_removed, .container_loaded =
		&container_loaded, };

/* ---------------------------  SESSION CALLBACKS  ------------------------- */
/**
 * This callback is called when an attempt to login has succeeded or failed.
 *
 * @sa sp_session_callbacks#logged_in
 */
static void SP_CALLCONV logged_in(sp_session *sess, sp_error error) {
  if (SP_ERROR_OK != error) {
    log_error("jahspotify", "logged_in", "Login failed: %s", sp_error_message(error));
    signalLoggedIn(0);
    return;
  }
  pthread_mutex_lock(&g_spotify_mutex);
  sp_playlistcontainer *pc = sp_session_playlistcontainer(sess);
  sp_playlistcontainer_add_callbacks(sp_session_playlistcontainer(g_sess), &pc_callbacks, NULL );
  
  log_debug("jahspotify", "logged_in", "Login Success: %d", sp_playlistcontainer_num_playlists(pc));
  signalLoggedIn(1);
  log_debug("jahspotify", "logged_in", "All done");
  pthread_mutex_unlock(&g_spotify_mutex);
}

static void SP_CALLCONV credentials_blob_updated(sp_session *session, const char *blob) {
  signalBlobUpdated(blob);
}

static void SP_CALLCONV logged_out(sp_session *sess) {
  log_debug("jahspotify", "logged_out", "Logged out");
  signalLoggedOut();
  if (g_stop_after_logout) {
    pthread_mutex_lock(&g_notify_mutex);
    g_stop = 1;
    g_notify_do = 1;
    pthread_cond_signal(&g_notify_cond);
    pthread_mutex_unlock(&g_notify_mutex);
  }
}

/**
 * This callback is called from an internal libspotify thread to ask us to
 * reiterate the main loop.
 *
 * We notify the main thread using a condition variable and a protected variable.
 *
 * @sa sp_session_callbacks#notify_main_thread
 */
static void SP_CALLCONV notify_main_thread(sp_session *sess) {
  pthread_mutex_lock(&g_notify_mutex);
  g_notify_do = 1;
  pthread_cond_signal(&g_notify_cond);
  pthread_mutex_unlock(&g_notify_mutex);
}

/**
 * This callback is used from libspotify whenever there is PCM data available.
 *
 * @sa sp_session_callbacks#music_delivery
 */
static int SP_CALLCONV music_delivery(sp_session *sess, const sp_audioformat *format, const void *frames, int num_frames) {
  if (num_frames == 0) return 0; // Audio discontinuity, do nothing
  
  JNIEnv* env = NULL;
  if (!retrieveEnv((JNIEnv*) &env)) return 0;
  
  invokeVoidMethod_II(env, g_playbackListener, "setAudioFormat", (jint) format->sample_rate, (jint) format->channels);
  
  int sampleSize = 2 * format->channels;
  int numBytes = num_frames * sampleSize;
  
  jbyteArray byteArray = (*env)->NewByteArray(env, numBytes);
  
  (*env)->SetByteArrayRegion(env, byteArray, 0, numBytes, (jbyte*) frames);
  int buffered;
  invokeIntMethod_B(env, g_playbackListener, "addToBuffer", &buffered, byteArray);
  
  (*env)->DeleteLocalRef(env, byteArray);
  return buffered;
}

/**
 * This callback is used from libspotify when the current track has ended
 *
 * @sa sp_session_callbacks#end_of_track
 */
static void SP_CALLCONV end_of_track(sp_session *sess) {
  pthread_mutex_lock(&g_notify_mutex);
  g_playback_done = 1;
  pthread_cond_signal(&g_notify_cond);
  pthread_mutex_unlock(&g_notify_mutex);
}

/**
 * Callback called when libspotify has new metadata available
 *
 * Not used in this example (but available to be able to reuse the session.c file
 * for other examples.)
 *
 * @sa sp_session_callbacks#metadata_updated
 */
static void SP_CALLCONV metadata_updated(sp_session *sess) {
	log_debug("jahspotify", "metadata_updated", "Metadata updated");
	checkLoaded();
}

/**
 * Notification that some other connection has started playing on this account.
 * Playback has been stopped.
 *
 * @sa sp_session_callbacks#play_token_lost
 */
static void SP_CALLCONV play_token_lost(sp_session *sess) {
	log_error("jahspotify", "play_token_lost", "Play token lost");
	signalPlayTokenLost();
}

static void SP_CALLCONV userinfo_updated(sp_session *sess) {
	log_debug("jahspotify", "userinfo_updated", "User information updated");
}

static void SP_CALLCONV log_message(sp_session *session, const char *data) {
	log_debug("jahspotify", "log_message", "Spotify log message: %s", data);
}

static void SP_CALLCONV connection_error(sp_session *session, sp_error error) {
	log_error("jahspotify", "connection_error", "Error: %s", sp_error_message(error));
	// FIXME: should propagate this to java land
	if (error == SP_ERROR_OK) {
		signalConnected();
	} else {
		log_error("jahspotify", "connection_error", "Unhandled error: %s", sp_error_message(error));
	}
}

static void SP_CALLCONV streaming_error(sp_session *session, sp_error error) {
	log_error("jahspotify", "streaming_error", "Streaming error: %s", sp_error_message(error));
	// FIXME: should propagate this to java land
}

static void SP_CALLCONV start_playback(sp_session *session) {
	log_debug("jahspotify", "start_playback", "Next playback about to start, initiating pre-load sequence");
	startPlaybackSignalled();
}

static void SP_CALLCONV message_to_user(sp_session *session, const char *data) {
	log_debug("jahspotify", "message_to_user", "Message to user: ", data);
}

/**
 * The session callbacks
 */
static sp_session_callbacks session_callbacks = { .message_to_user = &message_to_user, .logged_in = &logged_in, .logged_out = &logged_out, .notify_main_thread =
		&notify_main_thread, .music_delivery = &music_delivery, .metadata_updated = &metadata_updated, .play_token_lost = &play_token_lost, .log_message =
		log_message, .end_of_track = &end_of_track, .userinfo_updated = &userinfo_updated, .connection_error = &connection_error, .streaming_error =
		&streaming_error, .start_playback = &start_playback, .credentials_blob_updated = &credentials_blob_updated };

static sp_session_config spconfig = { .api_version = SPOTIFY_API_VERSION, .cache_location = "", // set in main
		.settings_location = "", // set in main
		.application_key = g_appkey, .application_key_size = 0, // Set in main()
		.user_agent = "jahspotify/0.0.1", .callbacks = &session_callbacks, NULL , };

static void SP_CALLCONV searchCompleteCallback(sp_search *result, void *userdata) {
	int32_t *token = (int32_t*) userdata;

	if (sp_search_error(result) == SP_ERROR_OK) {
		signalSearchComplete(result, *token);
	} else {
		log_error("jahspotify", "searchCompleteCallback", "Search completed with error: %s\n", sp_error_message(sp_search_error(result)));
	}
}

JNIEXPORT void JNICALL Java_jahspotify_impl_JahSpotifyImpl_nativeInitiateSearch(JNIEnv *env, jobject obj, jint javaToken, jobject javaNativeSearchParameters) {
	char *nativeQuery;
	int32_t *token = calloc(1, sizeof(int32_t));
	int32_t numAlbums;
	int32_t albumOffset;
	int32_t numArtists;
	int32_t artistOffset;
	int32_t numTracks;
	int32_t trackOffset;
	int32_t numPlaylists;
	int32_t playlistOffset;
	int32_t suggest;
	jint value;
	jboolean bValue;

	*token = javaToken;

	getObjectIntField(env, javaNativeSearchParameters, "numAlbums", &value);
	numAlbums = value;
	getObjectIntField(env, javaNativeSearchParameters, "albumOffset", &value);
	albumOffset = value;
	getObjectIntField(env, javaNativeSearchParameters, "numArtists", &value);
	numArtists = value;
	getObjectIntField(env, javaNativeSearchParameters, "artistOffset", &value);
	artistOffset = value;
	getObjectIntField(env, javaNativeSearchParameters, "numTracks", &value);
	numTracks = value;
	getObjectIntField(env, javaNativeSearchParameters, "trackOffset", &value);
	trackOffset = value;
	getObjectIntField(env, javaNativeSearchParameters, "numPlaylists", &value);
	numPlaylists = value;
	getObjectIntField(env, javaNativeSearchParameters, "playlistOffset", &value);
	playlistOffset = value;
	getObjectBoolField(env, javaNativeSearchParameters, "suggest", &bValue);
	suggest = bValue == JNI_TRUE ? 1 : 0;

	if (createNativeString(env, getObjectStringField(env, javaNativeSearchParameters, "_query"), &nativeQuery) != 1) {
		// FIXME: Handle error
	}

	sp_search_type type = suggest ? SP_SEARCH_SUGGEST : SP_SEARCH_STANDARD;
	sp_search_create(g_sess, nativeQuery, trackOffset, numTracks, albumOffset, numAlbums, artistOffset, numArtists, playlistOffset,
			numPlaylists, type, searchCompleteCallback, token);
}

JNIEXPORT jboolean JNICALL Java_jahspotify_impl_JahSpotifyImpl_registerNativeMediaLoadedListener(JNIEnv *env, jobject obj, jobject mediaLoadedListener) {
	g_mediaLoadedListener = (*env)->NewGlobalRef(env, mediaLoadedListener);
	log_debug("jahspotify", "registerNativeMediaLoadedListener", "Registered media loaded listener: 0x%x\n", (int) g_mediaLoadedListener);
	return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_jahspotify_impl_JahSpotifyImpl_registerNativeSearchCompleteListener(JNIEnv *env, jobject obj, jobject searchCompleteListener) {
	g_searchCompleteListener = (*env)->NewGlobalRef(env, searchCompleteListener);
	log_debug("jahspotify", "registerNativeSearchCompleteListener", "Registered search complete listener: 0x%x\n", (int) g_searchCompleteListener);
	return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_jahspotify_impl_JahSpotifyImpl_registerNativePlaybackListener(JNIEnv *env, jobject obj, jobject playbackListener) {
	g_playbackListener = (*env)->NewGlobalRef(env, playbackListener);
	log_debug("jahspotify", "registerNativePlaybackListener", "Registered playback listener: 0x%x\n", (int) g_playbackListener);
	return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_jahspotify_impl_JahSpotifyImpl_registerNativeConnectionListener(JNIEnv *env, jobject obj, jobject connectionListener) {
	g_connectionListener = (*env)->NewGlobalRef(env, connectionListener);
	log_debug("jahspotify", "registerNativeConnectionListener", "Registered connection listener: 0x%x\n", (int) g_connectionListener);
	return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL Java_jahspotify_impl_JahSpotifyImpl_unregisterListeners(JNIEnv *env, jobject obj) {
	if (g_mediaLoadedListener) {
		(*env)->DeleteGlobalRef(env, g_mediaLoadedListener);
		g_mediaLoadedListener = NULL;
	}

	if (g_searchCompleteListener) {
		(*env)->DeleteGlobalRef(env, g_searchCompleteListener);
		g_searchCompleteListener = NULL;
	}

	if (g_connectionListener) {
		(*env)->DeleteGlobalRef(env, g_connectionListener);
		g_connectionListener = NULL;
	}
	return JNI_TRUE;
}

JNIEXPORT jobject JNICALL Java_jahspotify_impl_JahSpotifyImpl_retrieveUser(JNIEnv *env, jobject obj) {
	sp_user *user = sp_session_user(g_sess);
	const char *value = NULL;
	int country = 0;

	log_error("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_retrieveUser", "Retrieving user");

	int count = 0;
	while (!sp_user_is_loaded(user) && count < 4) {
		usleep(250);
		count++;
	}

	if (count == 4) {
		log_warn("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_retrieveUser", "Timeout while waiting for user to load");
		return NULL ;
	}

	jobject userInstance = createInstance(env, "jahspotify/media/User");
	if (!userInstance) {
		log_error("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_retrieveUser", "Could not create instance of jahspotify.media.User");
		return NULL ;
	}

	if (sp_user_is_loaded(user)) {
		log_error("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_retrieveUser", "User is loaded");
		value = sp_user_display_name(user);
		if (value) {
			setObjectStringField(env, userInstance, "fullName", value);
		}
		value = sp_user_canonical_name(user);
		if (value) {
			setObjectStringField(env, userInstance, "userName", value);
		}
		value = sp_user_display_name(user);
		if (value) {
			setObjectStringField(env, userInstance, "displayName", value);
		}

		// Country encoded in an integer 'SE' = 'S' << 8 | 'E'
		country = sp_session_user_country(g_sess);
		char countryStr[] = "  ";
		countryStr[0] = (byte) (country >> 8);
		countryStr[1] = (byte) country;
		setObjectStringField(env, userInstance, "country", countryStr);
		return userInstance;
	}
	(*env)->DeleteLocalRef(env, userInstance);

	return NULL ;

}

char* createLinkStr(sp_link *link) {
	char *linkStr = calloc(1, sizeof(char) * (1024));
	sp_link_as_string(link, linkStr, 1024);
	return linkStr;
}

jobject createJLinkInstance(JNIEnv *env, sp_link *link) {
	if (!link) return NULL ;
	jobject linkInstance = NULL;
	jmethodID jMethod = NULL;

	char *linkStr = createLinkStr(link);

	jstring jString = (*env)->NewStringUTF(env, linkStr);

	jMethod = (*env)->GetStaticMethodID(env, g_linkClass, "create", "(Ljava/lang/String;)Ljahspotify/media/Link;");

	linkInstance = (*env)->CallStaticObjectMethod(env, g_linkClass, jMethod, jString);

	if (!linkInstance) {
		log_error("jahspotify", "createJLinkInstance", "Could not create instance of jahspotify.media.Link");
		goto exit;
	}

	exit: if (linkStr) {
		free(linkStr);
	}
	if (jString) (*env)->DeleteLocalRef(env, jString);
	return linkInstance;

}

jobject createJPlaylistInstance(JNIEnv *env, sp_link* link, const char* name, sp_link* image) {
	jobject linkInstance = createJLinkInstance(env, link);
	jobject imageLinkInstance = createJLinkInstance(env, image);

	jstring jString = (*env)->NewStringUTF(env, name);

	jobject playlistInstance = NULL;
	jmethodID jMethod = NULL;

	jMethod = (*env)->GetStaticMethodID(env, g_playlistCLass, "create",
			"(Ljahspotify/media/Link;Ljava/lang/String;Ljahspotify/media/Link;)Ljahspotify/media/Playlist;");
	playlistInstance = (*env)->CallStaticObjectMethod(env, g_linkClass, jMethod, linkInstance, jString, imageLinkInstance);

	if (!playlistInstance) {
		log_error("jahspotify", "createJPlaylistInstance", "Could not create instance of jahspotify.media.Playlist");
		goto exit;
	}

	exit: if (jString) (*env)->DeleteLocalRef(env, jString);
	if (linkInstance) (*env)->DeleteLocalRef(env, linkInstance);
	if (imageLinkInstance) (*env)->DeleteLocalRef(env, imageLinkInstance);
	return playlistInstance;
}

jobject createJTrackInstance(JNIEnv *env, sp_track *track) {
	jclass jClass;
	jobject trackInstance;

	jClass = (*env)->FindClass(env, "jahspotify/media/Track");
	if (jClass == NULL ) {
		log_error("jahspotify", "createJTrackInstance", "Could not load jahnotify.media.Track");
		return NULL ;
	}

	trackInstance = createInstanceFromJClass(env, jClass);
	if (!trackInstance) {
		log_error("jahspotify", "createJTrackInstance", "Could not create instance of jahspotify.media.Track");
		return NULL ;
	}

	if (sp_track_is_loaded(track))
		populateJTrackInstance(env, trackInstance, track);
	else
		addLoading((*env)->NewGlobalRef(env, trackInstance), track, NULL, NULL, 0);

	return trackInstance;
}

void populateJTrackInstance(JNIEnv *env, jobject trackInstance, sp_track *track) {
	jclass jClass;

	jClass = (*env)->FindClass(env, "jahspotify/media/Track");
	if (jClass == NULL ) {
		log_error("jahspotify", "populateJTrackInstance", "Could not load jahnotify.media.Track");
		sp_track_release(track);
		return ;
	}

	sp_link *trackLink = sp_link_create_from_track(track, 0);

	if (trackLink) {
		sp_link_add_ref(trackLink);
		jobject trackJLink = createJLinkInstance(env, trackLink);
		setObjectObjectField(env, trackInstance, "id", "Ljahspotify/media/Link;", trackJLink);

		setObjectStringField(env, trackInstance, "title", sp_track_name(track));
		setObjectIntField(env, trackInstance, "length", sp_track_duration(track));
		setObjectIntField(env, trackInstance, "popularity", sp_track_popularity(track));
		setObjectIntField(env, trackInstance, "trackNumber", sp_track_index(track));

		sp_album *album = sp_track_album(track);
		if (album) {
			sp_album_add_ref(album);
			sp_link *albumLink = sp_link_create_from_album(album);
			if (albumLink) {
				sp_link_add_ref(albumLink);

				jobject albumJLink = createJLinkInstance(env, albumLink);

				jmethodID jMethod = (*env)->GetMethodID(env, jClass, "setAlbum", "(Ljahspotify/media/Link;)V");

				if (jMethod == NULL ) {
					log_error("jahspotify", "populateJTrackInstance", "Could not load method setAlbum(link) on class Track");
					sp_track_release(track);
					return ;
				}

				// set it on the track
				(*env)->CallVoidMethod(env, trackInstance, jMethod, albumJLink);

				sp_link_release(albumLink);

			}
			sp_album_release(album);
		}

		int numArtists = sp_track_num_artists(track);
		if (numArtists > 0) {
			jmethodID jMethod = (*env)->GetMethodID(env, jClass, "addArtist", "(Ljahspotify/media/Link;)V");

			if (jMethod == NULL ) {
				log_error("jahspotify", "populateJTrackInstance", "Could not load method addArtist(link) on class Track");
				sp_track_release(track);
				return ;
			}

			int i = 0;
			for (i = 0; i < numArtists; i++) {
				sp_artist *artist = sp_track_artist(track, i);
				if (artist) {
					sp_artist_add_ref(artist);

					sp_link *artistLink = sp_link_create_from_artist(artist);
					if (artistLink) {
						sp_link_add_ref(artistLink);

						jobject artistJLink = createJLinkInstance(env, artistLink);

						// set it on the track
						(*env)->CallVoidMethod(env, trackInstance, jMethod, artistJLink);

						sp_link_release(artistLink);

					}
					sp_artist_release(artist);
				}
			}
		}

		sp_link_release(trackLink);
	}
	invokeVoidMethod_Z(env, trackInstance, "setLoaded", JNI_TRUE);
	sp_track_release(track);
}

char* toHexString(byte* bytes) {
	char ls_hex[3] = "";
	int i = 0;
	int j = 0;
	char *finalHash = calloc(1, sizeof(char) * 41);
	byte *theBytes = bytes;

	memset(ls_hex, '\0', 3);

	j = 0;
	for (i = 0; i < 20; i++) {
		sprintf(ls_hex, "%.2X", *theBytes);
		theBytes++;
		finalHash[j++] = ls_hex[0];
		finalHash[j++] = ls_hex[1];
	}
	finalHash[40] = '\0';
	return finalHash;
}

void SP_CALLCONV artistBrowseCompleteCallback(sp_artistbrowse *result, void *userdata) {
	jobject instance = (jobject) userdata;
	signalArtistBrowseLoaded(result, instance);
}

void SP_CALLCONV imageLoadedCallback(sp_image *image, void *userdata) {
	sp_image_remove_load_callback(image, imageLoadedCallback, userdata);
	jobject instance = (jobject) userdata;
	signalImageLoaded(image, instance);
}
/*
 void trackLoadedCallback(sp_track *track, void *userdata)
 {
 int32_t *token = (int32_t*)userdata;
 signalTrackLoaded(track, *token);
 }*/

void SP_CALLCONV albumBrowseCompleteCallback(sp_albumbrowse *result, void *userdata) {
	jobject instance = (jobject) userdata;
	signalAlbumBrowseLoaded(result, instance);
}

void populateJAlbumInstanceFromAlbumBrowse(JNIEnv *env, sp_album *album, sp_albumbrowse *albumBrowse, jobject albumInstance) {
	jclass albumJClass;

	sp_album_add_ref(album);
	sp_albumbrowse_add_ref(albumBrowse);

	albumJClass = (*env)->FindClass(env, "jahspotify/media/Album");
	if (albumJClass == NULL ) {
		log_error("jahspotify", "populateJAlbumInstanceFromAlbumBrowse", "Could not load jahnotify.media.Album");
		return;
	}

	int numTracks = sp_albumbrowse_num_tracks(albumBrowse);
	if (numTracks > 0) {
		// Add each track to the album - also pass in the disk as need be
		jmethodID addTrackJMethodID = (*env)->GetMethodID(env, albumJClass, "addTrack", "(ILjahspotify/media/Link;)V");
		int i = 0;
		for (i = 0; i < numTracks; i++) {
			sp_track *track = sp_albumbrowse_track(albumBrowse, i);

			if (track) {
				sp_track_add_ref(track);

				sp_link *trackLink = sp_link_create_from_track(track, 0);
				if (trackLink) {
					sp_link_add_ref(trackLink);
					jobject trackJLink = createJLinkInstance(env, trackLink);
					(*env)->CallVoidMethod(env, albumInstance, addTrackJMethodID, sp_track_disc(track), trackJLink);
					sp_link_release(trackLink);
				}
			}
		}
	}

	int numCopyrights = sp_albumbrowse_num_copyrights(albumBrowse);
	if (numCopyrights > 0) {
		// Add copyrights to album
		jmethodID addCopyrightMethodID = (*env)->GetMethodID(env, albumJClass, "addCopyright", "(Ljava/lang/String;)V");
		int i = 0;
		for (i = 0; i < numCopyrights; i++) {
			const char *copyright = sp_albumbrowse_copyright(albumBrowse, i);
			if (copyright) {
				jstring str = (*env)->NewStringUTF(env, copyright);
				(*env)->CallVoidMethod(env, albumInstance, addCopyrightMethodID, str);
				(*env)->DeleteLocalRef(env, str);
			}
		}
	}

	const char *review = sp_albumbrowse_review(albumBrowse);
	if (review) {
		setObjectStringField(env, albumInstance, "review", review);
	}

	sp_album_release(album);
	sp_albumbrowse_release(albumBrowse);

}

jobject createJAlbumInstance(JNIEnv *env, sp_album *album, int browse) {
	jobject albumInstance;
	jclass albumJClass;

	albumJClass = (*env)->FindClass(env, "jahspotify/media/Album");
	if (albumJClass == NULL ) {
		log_error("jahspotify", "createJAlbumInstance", "Could not load jahnotify.media.Album");
		sp_album_release(album);
		return NULL ;
	}

	albumInstance = createInstanceFromJClass(env, albumJClass);
	if (!albumInstance) {
		log_error("jahspotify", "createJAlbumInstance", "Could not create instance of jahspotify.media.Album");
		sp_album_release(album);
		return NULL ;
	}

	if (sp_album_is_loaded(album)) {
		populateJAlbumInstance(env, albumInstance, album, browse);
	} else {
		addLoading((*env)->NewGlobalRef(env, albumInstance), NULL, album, NULL, browse);
	}
	return albumInstance;
}
void populateJAlbumInstance(JNIEnv *env, jobject albumInstance, sp_album *album, int browse) {
	// By now it looks like the album will be loaded
	sp_link *albumLink = sp_link_create_from_album(album);

	if (albumLink) {
		sp_link_add_ref(albumLink);

		jobject albumJLink = createJLinkInstance(env, albumLink);
		setObjectObjectField(env, albumInstance, "id", "Ljahspotify/media/Link;", albumJLink);

		sp_link_release(albumLink);
	}

	setObjectStringField(env, albumInstance, "name", sp_album_name(album));
	setObjectIntField(env, albumInstance, "year", sp_album_year(album));

	sp_albumtype albumType = sp_album_type(album);

	jclass albumTypeJClass = (*env)->FindClass(env, "jahspotify/media/AlbumType");
	jmethodID jMethod = (*env)->GetStaticMethodID(env, albumTypeJClass, "fromOrdinal", "(I)Ljahspotify/media/AlbumType;");
	jobject albumTypeEnum = (jobjectArray) (*env)->CallStaticObjectMethod(env, albumTypeJClass, jMethod, (int) albumType);
	setObjectObjectField(env, albumInstance, "type", "Ljahspotify/media/AlbumType;", albumTypeEnum);

	sp_link *albumCoverLink = sp_link_create_from_album_cover(album, SP_IMAGE_SIZE_NORMAL);
	if (albumCoverLink) {
		sp_link_add_ref(albumCoverLink);

		jobject albumCoverJLink = createJLinkInstance(env, albumCoverLink);
		setObjectObjectField(env, albumInstance, "cover", "Ljahspotify/media/Link;", albumCoverJLink);

//		sp_image *albumCoverImage = sp_image_create_from_link(g_sess, albumCoverLink);
//		if (albumCoverImage) {
//			sp_image_add_ref(albumCoverImage);
//        	sp_image_add_load_callback(albumCoverImage,imageLoadedCallback,NULL);
//		}
//		sp_link_release(albumCoverLink);
	}

	sp_artist *artist = sp_album_artist(album);
	if (artist) {
		sp_artist_add_ref(artist);

		sp_link *artistLink = sp_link_create_from_artist(artist);

		if (artistLink) {
			sp_link_add_ref(artistLink);

			jobject artistJLink = createJLinkInstance(env, artistLink);

			setObjectObjectField(env, albumInstance, "artist", "Ljahspotify/media/Link;", artistJLink);

			sp_link_release(artistLink);
		}

		sp_artist_release(artist);
	}

	if (browse)
		sp_albumbrowse_create(g_sess, album, albumBrowseCompleteCallback, (*env)->NewGlobalRef(env, albumInstance));
	else
		invokeVoidMethod_Z(env, albumInstance, "setLoaded", JNI_TRUE);

	sp_album_release(album);
}

void populateJArtistInstanceFromArtistBrowse(JNIEnv *env, sp_artistbrowse *artistBrowse, jobject artistInstance) {
	log_debug("jahspotify", "populateJArtistInstanceFromArtistBrowse", "Populating artist browse instance");

	sp_artistbrowse_add_ref(artistBrowse);

	int numSimilarArtists = sp_artistbrowse_num_similar_artists(artistBrowse);
	jclass jClass = (*env)->FindClass(env, "jahspotify/media/Artist");
	if (numSimilarArtists > 0) {
		jmethodID jMethod = (*env)->GetMethodID(env, jClass, "addSimilarArtist", "(Ljahspotify/media/Link;)V");

		if (jMethod == NULL ) {
			log_error("jahspotify", "populateJArtistInstanceFromArtistBrowse", "Could not load method addSimilarArtist(link) on class Artist");
			sp_artistbrowse_release(artistBrowse);
			return;
		}

		// Load the artist links
		int count = 0;
		for (count = 0; count < numSimilarArtists; count++) {
			sp_artist *similarArtist = sp_artistbrowse_similar_artist(artistBrowse, count);
			if (similarArtist) {
				sp_artist_add_ref(similarArtist);

				sp_link *similarArtistLink = sp_link_create_from_artist(similarArtist);

				if (similarArtistLink) {
					sp_link_add_ref(similarArtistLink);

					jobject similarArtistJLink = createJLinkInstance(env, similarArtistLink);

					// set it on the track
					(*env)->CallVoidMethod(env, artistInstance, jMethod, similarArtistJLink);

					sp_link_release(similarArtistLink);
				}

				sp_artist_release(similarArtist);
			}
		}
	}

	int numPortraits = sp_artistbrowse_num_portraits(artistBrowse);

	if (numPortraits > 0) {
		jmethodID jMethod = (*env)->GetMethodID(env, jClass, "addPortrait", "(Ljahspotify/media/Link;)V");

		if (jMethod == NULL ) {
			log_error("jahspotify", "populateJArtistInstanceFromArtistBrowse", "Could not load method addAlbum(link) on class Artist");
			sp_artistbrowse_release(artistBrowse);
			return;
		}

		int count = 0;

		for (count = 0; count < numPortraits; count++) {
			// Load portrait url
			const byte *portraitURI = sp_artistbrowse_portrait(artistBrowse, count);

			if (portraitURI) {
				char *portraitURIStr = toHexString((byte*) portraitURI);
				const char spotifyURI[] = "spotify:image:";
				int len = strlen(spotifyURI) + strlen(portraitURIStr);
				char *dest = calloc(1, len + 1);
				dest[0] = 0;
				strcat(dest, spotifyURI);
				strcat(dest, portraitURIStr);

				sp_link *portraitLink = sp_link_create_from_string(dest);
				if (portraitLink) {
					// sp_image *portrait = sp_image_create_from_link(g_sess,portraitLink);
					// if (portrait)
					// {
					// sp_image_add_ref(portrait);
					// sp_image_add_load_callback(portrait,imageLoadedCallback,NULL);
					//}

					sp_link_add_ref(portraitLink);

					jobject portraitJLlink = createJLinkInstance(env, portraitLink);

					// setObjectObjectField(env,artistInstance,"portrait","Ljahspotify/media/Link;",portraitJLlink);
					(*env)->CallVoidMethod(env, artistInstance, jMethod, portraitJLlink);

					sp_link_release(portraitLink);
				}

				free(dest);
				free(portraitURIStr);

			}
		}
	}

	int numAlbums = sp_artistbrowse_num_albums(artistBrowse);
	if (numAlbums > 0) {
		jmethodID jMethod = (*env)->GetMethodID(env, jClass, "addAlbum", "(Ljahspotify/media/Link;)V");

		if (jMethod == NULL ) {
			log_error("jahspotify", "populateJArtistInstanceFromArtistBrowse", "Could not load method addAlbum(link) on class Artist");
			sp_artistbrowse_release(artistBrowse);
			return;
		}

		int count = 0;
		for (count = 0; count < numAlbums; count++) {
			sp_album *album = sp_artistbrowse_album(artistBrowse, count);
			if (album && sp_album_is_available(album)) {
				sp_album_add_ref(album);
				sp_link *albumLink = sp_link_create_from_album(album);
				if (albumLink) {
					sp_link_add_ref(albumLink);
					jobject albumJLink = createJLinkInstance(env, albumLink);
					// set it on the track
					(*env)->CallVoidMethod(env, artistInstance, jMethod, albumJLink);
					sp_link_release(albumLink);
				}
				sp_album_release(album);
			}
		}
	}

	int numTopTracks = sp_artistbrowse_num_tophit_tracks(artistBrowse);
	if (numTopTracks > 0) {
		jmethodID jMethod = (*env)->GetMethodID(env, jClass, "addTopHitTrack", "(Ljahspotify/media/Link;)V");

		if (jMethod == NULL ) {
			log_error("jahspotify", "populateJArtistInstanceFromArtistBrowse", "Could not load method addTopHitTrack(link) on class Artist");
			sp_artistbrowse_release(artistBrowse);
			return;
		}

		int count = 0;
		for (count = 0; count < numTopTracks; count++) {
			sp_track *track = sp_artistbrowse_tophit_track(artistBrowse, count);
			if (track && sp_track_get_availability(g_sess, track) == SP_TRACK_AVAILABILITY_AVAILABLE) {
				sp_track_add_ref(track);
				sp_link *trackLink = sp_link_create_from_track(track, 0);
				if (trackLink) {
					sp_link_add_ref(trackLink);
					jobject albumJLink = createJLinkInstance(env, trackLink);
					// set it on the track
					(*env)->CallVoidMethod(env, artistInstance, jMethod, albumJLink);
					sp_link_release(trackLink);
				}
				sp_track_release(track);
			}
		}
	}

	const char *bios = sp_artistbrowse_biography(artistBrowse);

	if (bios) {
		setObjectStringField(env, artistInstance, "bios", bios);
	}

	sp_artistbrowse_release(artistBrowse);
}

jobject createJArtistInstance(JNIEnv *env, sp_artist *artist, int browse) {
	jobject artistInstance = NULL;

	sp_artist_add_ref(artist);

	jclass jClass = (*env)->FindClass(env, "jahspotify/media/Artist");

	if (jClass == NULL ) {
		log_error("jahspotify", "createJArtistInstance", "Could not load jahnotify.media.Artist");
		sp_artist_release(artist);
		return NULL ;
	}

	artistInstance = createInstanceFromJClass(env, jClass);

	if (sp_artist_is_loaded(artist))
		populateJArtistInstance(env, artistInstance, artist, browse);
	else
		addLoading((*env)->NewGlobalRef(env, artistInstance), NULL, NULL, artist, browse);
	return artistInstance;
}

void populateJArtistInstance(JNIEnv *env, jobject artistInstance, sp_artist *artist, int browse) {
	sp_link *artistLink = NULL;
	artistLink = sp_link_create_from_artist(artist);

	if (artistLink) {
		sp_link_add_ref(artistLink);

		jobject artistJLink = createJLinkInstance(env, artistLink);
		setObjectObjectField(env, artistInstance, "id", "Ljahspotify/media/Link;", artistJLink);

		sp_link_release(artistLink);

		setObjectStringField(env, artistInstance, "name", sp_artist_name(artist));

		if (browse > 0)
			sp_artistbrowse_create(g_sess, artist, browse == 1 ? SP_ARTISTBROWSE_NO_TRACKS : SP_ARTISTBROWSE_NO_ALBUMS, artistBrowseCompleteCallback,
					(*env)->NewGlobalRef(env, artistInstance));
		else
			invokeVoidMethod_Z(env, artistInstance, "setLoaded", JNI_TRUE);
	}

	sp_artist_release(artist);
}

jobject createJPlaylist(JNIEnv *env, jobject playlistInstance, sp_playlist *playlist) {
	jmethodID jMethod;
	jclass jClass;

	jClass = (*env)->FindClass(env, "jahspotify/media/Playlist");
	if (!playlistInstance) {
		if (jClass == NULL ) {
			log_error("jahspotify", "createJPlaylist", "Could not load jahnotify.media.Playlist");
			return NULL ;
		}

		playlistInstance = createInstanceFromJClass(env, jClass);
		if (!playlistInstance) {
			log_error("jahspotify", "createJPlaylist", "Could not create instance of jahspotify.media.Playlist");
			return NULL ;
		}
	}

	// Return the unloaded instance.
	if (!sp_playlist_is_loaded(playlist)) {
		sp_playlist_add_callbacks(playlist, &pl_callbacks, (*env)->NewGlobalRef(env, playlistInstance));
		return playlistInstance;
	}

	sp_link *playlistLink = sp_link_create_from_playlist(playlist);
	if (playlistLink) {
		jobject playlistJLink = createJLinkInstance(env, playlistLink);
		setObjectObjectField(env, playlistInstance, "id", "Ljahspotify/media/Link;", playlistJLink);
		sp_link_release(playlistLink);
	}

	setObjectStringField(env, playlistInstance, "name", sp_playlist_name(playlist));
	sp_user *owner = sp_playlist_owner(playlist);
	if (owner) {
		setObjectStringField(env, playlistInstance, "author", sp_user_display_name(owner));
		sp_user_release(owner);
	}

	// Lookup the method now - saves us looking it up for each iteration of the loop
	jMethod = (*env)->GetMethodID(env, jClass, "addTrack", "(Ljahspotify/media/Link;)V");
	if (jMethod == NULL ) {
		log_error("jahspotify", "createJPlaylist", "Could not load method addTrack(track) on class Playlist");
		return NULL ;
	}
	jmethodID clearMethod = (*env)->GetMethodID(env, jClass, "clear", "()V");
	(*env)->CallVoidMethod(env, playlistInstance, clearMethod);

	int numTracks = sp_playlist_num_tracks(playlist);
	setObjectIntField(env, playlistInstance, "numTracks", numTracks);

	int trackCounter = 0;
	for (trackCounter = 0; trackCounter < numTracks; trackCounter++) {
		sp_track *track = sp_playlist_track(playlist, trackCounter);
		if (track && sp_track_get_availability(g_sess, track) <= SP_TRACK_AVAILABILITY_AVAILABLE) {
			sp_track_add_ref(track);
			sp_link *trackLink = sp_link_create_from_track(track, 0);
			if (trackLink) {
				sp_link_add_ref(trackLink);
				jobject trackJLink = createJLinkInstance(env, trackLink);
				// Add it to the playlist
				(*env)->CallVoidMethod(env, playlistInstance, jMethod, trackJLink);
				sp_link_release(trackLink);
			}
			sp_track_release(track);

		}
	}
	if (sp_playlist_is_loaded(playlist)) {
		invokeVoidMethod_Z(env, playlistInstance, "setLoaded", JNI_TRUE);
		signalPlaylistLoaded(playlistInstance);
	}
	return playlistInstance;
}

JNIEXPORT jobject JNICALL Java_jahspotify_impl_JahSpotifyImpl_retrieveArtist(JNIEnv *env, jobject obj, jstring uri, jint browse) {
	jobject artistInstance;
	const char *nativeUri = NULL;

	nativeUri = (*env)->GetStringUTFChars(env, uri, NULL );

	sp_link *link = sp_link_create_from_string(nativeUri);
	if (link) {
		sp_artist *artist = sp_link_as_artist(link);

		if (artist) {
			sp_artist_add_ref(artist);
			artistInstance = createJArtistInstance(env, artist, browse);
		}
		sp_link_release(link);
	}

	if (nativeUri) (*env)->ReleaseStringUTFChars(env, uri, nativeUri);

	return artistInstance;
}

JNIEXPORT jobject JNICALL Java_jahspotify_impl_JahSpotifyImpl_retrieveAlbum(JNIEnv *env, jobject obj, jstring uri, jboolean browse) {
	jobject albumInstance;
	const char *nativeUri = NULL;

	nativeUri = (*env)->GetStringUTFChars(env, uri, NULL );

	sp_link *link = sp_link_create_from_string(nativeUri);
	if (link) {
		sp_album *album = sp_link_as_album(link);

		if (album) {
			sp_album_add_ref(album);
			albumInstance = createJAlbumInstance(env, album, browse ? 1 : 0);
		}
		sp_link_release(link);
	}

	if (nativeUri) (*env)->ReleaseStringUTFChars(env, uri, nativeUri);

	return albumInstance;
}

JNIEXPORT jboolean JNICALL Java_jahspotify_impl_JahSpotifyImpl_nativeShutdown(JNIEnv *env, jobject obj) {
	sp_session_logout(g_sess);
	return JNI_TRUE;
}

JNIEXPORT jobject JNICALL Java_jahspotify_impl_JahSpotifyImpl_retrieveTrack(JNIEnv *env, jobject obj, jstring uri) {
	jobject trackInstance;
	const char *nativeUri = NULL;

	nativeUri = (*env)->GetStringUTFChars(env, uri, NULL );

	sp_link *link = sp_link_create_from_string(nativeUri);
	if (!link) {
		// hmm
		log_error("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_retrieveTrack", "Could not create link!");
		return JNI_FALSE;
	}

	sp_track *track = sp_link_as_track(link);
	sp_track_add_ref(track);

	trackInstance = createJTrackInstance(env, track);

	if (link) sp_link_release(link);
	if (nativeUri) (*env)->ReleaseStringUTFChars(env, uri, nativeUri);

	return trackInstance;
}

JNIEXPORT jobject JNICALL Java_jahspotify_impl_JahSpotifyImpl_retrievePlaylist(JNIEnv *env, jobject obj, jstring uri) {
	jobject playlistInstance;
	sp_playlist *playlist;
	const char *nativeUri = NULL;
	sp_link *link = NULL;

	if (uri) {
		nativeUri = (*env)->GetStringUTFChars(env, uri, NULL );

		log_debug("jahspotify", "retrievePlaylist", "Retrieving playlist: %s", nativeUri);

		link = sp_link_create_from_string(nativeUri);
		if (!link) {
			// hmm
			log_error("jahspotify", "retrievePlaylist", "Could not create link!");
			return JNI_FALSE;
		}

		playlist = sp_playlist_create(g_sess, link);
	} else {
		playlist = sp_session_starred_create(g_sess);
	}

	playlistInstance = createJPlaylist(env, NULL, playlist);
	if (!sp_playlist_is_loaded(playlist)) sp_playlist_add_callbacks(playlist, &pl_callbacks, (*env)->NewGlobalRef(env, playlistInstance));

	if (playlist) sp_playlist_release(playlist);
	if (link) sp_link_release(link);
	if (nativeUri) (*env)->ReleaseStringUTFChars(env, uri, nativeUri);

	return playlistInstance;
}

static void SP_CALLCONV toplistCallback(sp_toplistbrowse *result, void *userdata) {
	signalToplistComplete(result, (jobject) userdata);
}
JNIEXPORT jobject JNICALL Java_jahspotify_impl_JahSpotifyImpl_retrieveTopList(JNIEnv *env, jobject obj, jint type, jint countrycode) {
	jobject searchResult = createSearchResult(env);
	sp_toplistbrowse_create(g_sess, (int) type, countrycode == -1 ? SP_TOPLIST_REGION_EVERYWHERE : countrycode, NULL, toplistCallback, (*env)->NewGlobalRef(env, searchResult));
	return searchResult;
}

JNIEXPORT jobjectArray JNICALL Java_jahspotify_impl_JahSpotifyImpl_nativeReadTracks(JNIEnv *env, jobject obj, jobjectArray uris) {
	// For each track, read out the info and populate all of the info in the Track instance
	return NULL ;
}

JNIEXPORT jint JNICALL Java_jahspotify_impl_JahSpotifyImpl_nativePause(JNIEnv *env, jobject obj) {
	log_debug("jahspotify", "nativeResume", "Pausing playback");
	if (g_currenttrack) {
		sp_session_player_play(g_sess, 0);
	}
	return 0;
}

JNIEXPORT jint JNICALL Java_jahspotify_impl_JahSpotifyImpl_nativeResume(JNIEnv *env, jobject obj) {
	log_debug("jahspotify", "nativeResume", "Resuming playback");
	if (g_currenttrack) {
		sp_session_player_play(g_sess, 1);
	}
	return 0;
}

JNIEXPORT void JNICALL Java_jahspotify_impl_JahSpotifyImpl_readImage(JNIEnv *env, jobject obj, jstring uri, jobject imageInstance) {
	const char *nativeURI = (*env)->GetStringUTFChars(env, uri, NULL );
	sp_link *imageLink = sp_link_create_from_string(nativeURI);
	log_debug("jahspotify", "readImage", "Loading image: %s", nativeURI);

	if (imageLink) {
		sp_link_add_ref(imageLink);
		sp_image *image = sp_image_create_from_link(g_sess, imageLink);
		if (image) {
			imageInstance = (*env)->NewGlobalRef(env, imageInstance);
			// Reference is released by the image loaded callback
			if (sp_image_is_loaded(image)) {
				log_debug("jahspotify", "readImage", "Image already loaded, dont wait for callback.");
				signalImageLoaded(image, imageInstance);
			} else {
				sp_image_add_load_callback(image, imageLoadedCallback, (*env)->NewGlobalRef(env, imageInstance));
			}
		}
		sp_link_release(imageLink);
	} else {
		log_error("jahspotify", "readImage", "Image link is null");
	}

	if (nativeURI) (*env)->ReleaseStringUTFChars(env, uri, (char *) nativeURI);
}

JNIEXPORT void JNICALL Java_jahspotify_impl_JahSpotifyImpl_nativeTrackSeek(JNIEnv *env, jobject obj, jint offset) {
	log_debug("jahspotify", "nativeTrackSeek", "Seeking in track offset: %d", offset);
	sp_session_player_seek(g_sess, offset);
}


JNIEXPORT void JNICALL Java_jahspotify_impl_JahSpotifyImpl_nativeStopTrack(JNIEnv *env, jobject obj) {
  log_debug("jahspotify", "nativeStopTrack", "Stopping playback");
  pthread_mutex_lock(&g_notify_mutex);
  g_playback_stopped = 1;
  pthread_cond_signal(&g_notify_cond);
  pthread_mutex_unlock(&g_notify_mutex);
}

JNIEXPORT void JNICALL Java_jahspotify_impl_JahSpotifyImpl_setBitrate(JNIEnv * env, jobject obj, jint rate) {
  sp_session_preferred_bitrate(g_sess, rate);
}

static jint doPlay(const char *nativeURI) {
  
  log_debug("jahspotify", "nativePlayTrack", "Initiating play: %s", nativeURI);
  
  // For each track, read out the info and populate all of the info in the Track instance
  pthread_mutex_lock(&g_spotify_mutex);
  sp_link *link = sp_link_create_from_string(nativeURI);
  if (link) {
    sp_track *t = sp_link_as_track(link);
    
    if (!t) {
      log_error("jahspotify", "nativePlayTrack", "No track from link");
      pthread_mutex_unlock(&g_spotify_mutex);
      return -1;
    }
    
    int count = 0;
    while (!sp_track_is_loaded(t) && count < 4) {
      usleep(250);
      count++;
    }
    
    if (count == 4) {
      log_warn("jahspotify", "nativePlayTrack", "Track not loaded after 1 second, will have to wait for callback");
      pthread_mutex_unlock(&g_spotify_mutex);
      return -1;
    }
    
    if (sp_track_error(t) != SP_ERROR_OK) {
      log_debug("jahspotify", "nativePlayTrack", "Error with track: %s", sp_error_message(sp_track_error(t)));
      pthread_mutex_unlock(&g_spotify_mutex);
      return -1;
    }
    
    log_debug("jahspotify", "nativePlayTrack", "track name: %s duration: %d", sp_track_name(t), sp_track_duration(t));
    
    
    // If there is one playing, unload that now
    if (g_currenttrack) {
      // Unload the current track now
      sp_session_player_play(g_sess, 0);
      track_ended(JNI_TRUE);
    }
    
    sp_track_add_ref(t);
    
    sp_error result = sp_session_player_load(g_sess, t);
    int ret;
    
    if (sp_track_error(t) != SP_ERROR_OK) {
      log_error("jahspotify", "nativePlayTrack", "Issue loading track: %s", sp_error_message((sp_track_error(t))));
      ret = -1;
    } else {
      log_debug("jahspotify", "nativePlayTrack", "Track loaded: %s", (result == SP_ERROR_OK ? "yes" : "no"));
    
      // Update the global reference
      g_currenttrack = t;
      if (result != SP_ERROR_OK) {
        signalTrackStarted(nativeURI);
        track_ended(JNI_TRUE);
        ret = 0;
      } else {
        // Start playing the next track
        sp_session_player_play(g_sess, 1);
        log_debug("jahspotify", "nativePlayTrack", "Playing track");
      }
      sp_link_release(link);
      ret = 1;
    }
    pthread_mutex_unlock(&g_spotify_mutex);
    if (ret > 0) {
      signalTrackStarted(nativeURI);
    }
    return ret;
  } else {
    log_error("jahspotify", "nativePlayTrack", "Unable to load link at this point");
  }
  
  log_error("jahspotify", "nativePlayTrack", "Error starting play");
  
  return 0;
}

JNIEXPORT jint JNICALL Java_jahspotify_impl_JahSpotifyImpl_nativePlayTrack(JNIEnv *env, jobject obj, jstring uri) {
  const char *nativeURI = NULL;
  pthread_mutex_lock(&g_spotify_mutex);  
  nativeURI = (*env)->GetStringUTFChars(env, uri, NULL );
  jint result = doPlay(nativeURI);
  pthread_mutex_unlock(&g_spotify_mutex);  
  if (nativeURI) (*env)->ReleaseStringUTFChars(env, uri, (char *) nativeURI);
  return result;
}



/**
 * A track has ended. Remove it from the playlist.
 *
 * Called from the main loop when the music_delivery() callback has set g_playback_done.
 */
static void track_ended(jboolean forced) {
  log_debug("jahspotify", "track_ended", "Called");
  if (g_currenttrack) {
    log_debug("jahspotify", "track_ended", "current track exists");
    sp_link *link = sp_link_create_from_track(g_currenttrack, 0);
    char *trackLinkStr = NULL;
    if (link) {
      trackLinkStr = createLinkStr(link);
      sp_link_release(link);
    }
    if (forced) {
      log_debug("jahspotify", "track_ended", "unload session");
      sp_session_player_unload(g_sess);
    }
    log_debug("jahspotify", "track_ended", "track release");
    sp_track_release(g_currenttrack);
    g_currenttrack = NULL;
    log_debug("jahspotify", "track_ended", "signalling track ended");
    signalTrackEnded(trackLinkStr, forced);
    
    if (trackLinkStr) {
      free(trackLinkStr);
    }
  } else {
    log_debug("jahspotify", "track_ended", "no current track");
  }
}

JNIEXPORT jint JNICALL Java_jahspotify_impl_JahSpotifyImpl_nativeInitialize(JNIEnv *env, jobject obj, jstring cacheFolder) {
	sp_session *sp;
	sp_error err;
	int next_timeout = 0;

        pthread_mutexattr_t Attr;
        pthread_mutexattr_init(&Attr);
        pthread_mutexattr_settype(&Attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&g_spotify_mutex, &Attr);
	pthread_mutex_init(&g_notify_mutex, NULL );
	pthread_cond_init(&g_notify_cond, NULL );

	const char* nativeCacheFolder = (*env)->GetStringUTFChars(env, cacheFolder, NULL );

	log_debug("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_initialize", "Using the following cache and setting location: %s\n", nativeCacheFolder);
	spconfig.cache_location = nativeCacheFolder;
	spconfig.settings_location = nativeCacheFolder;

	/* Create session */
	spconfig.application_key_size = g_appkey_size;
	err = sp_session_create(&spconfig, &sp);

	if (SP_ERROR_OK != err) {
		log_error("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_initialize", "Unable to create session: %s\n", sp_error_message(err));
		return 1;
	}
	g_sess = sp;
	sp_session_set_volume_normalization(g_sess, 1);
	log_debug("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_initialize", "Session created 0x%x", sp);

	pthread_mutex_lock(&g_notify_mutex);

	g_stop = 0;
	for (;;) {
          if (next_timeout == 0) {
            signalInitialized(1);
            while (!g_notify_do && !g_playback_done)
              pthread_cond_wait(&g_notify_cond, &g_notify_mutex);
          } else {
            struct timespec ts;
            
#if _POSIX_TIMERS > 0
            clock_gettime ( CLOCK_REALTIME, &ts );
#else
            struct timeval tv;
            gettimeofday(&tv, NULL );
            //TIMEVAL_TO_TIMESPEC ( &tv, &ts );
            (&ts)->tv_sec = (&tv)->tv_sec;
            (&ts)->tv_nsec = (&tv)->tv_usec * 1000;
            ///TIMEVAL_TO_TIMESPEC ( &tv, &ts );
#endif
            ts.tv_sec += next_timeout / 1000;
            ts.tv_nsec += (next_timeout % 1000) * 1000000;
            
            if (!g_notify_do) // Only wait if we know we have nothing to do.
              pthread_cond_timedwait(&g_notify_cond, &g_notify_mutex, &ts);
          }
          
          g_notify_do = 0;
          bool playback_done = g_playback_done;
          bool playback_stopped = g_playback_stopped;
          g_playback_done = 0;
          g_playback_stopped = 0;
          pthread_mutex_unlock(&g_notify_mutex);
          pthread_mutex_lock(&g_spotify_mutex);
          if (playback_done) {
            track_ended(JNI_FALSE);
          } else if (playback_stopped) {
            track_ended(JNI_TRUE);
          }
          sp_connectionstate conn_state = sp_session_connectionstate(sp);
          if (!conn_state) {
            log_warn("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_initialize", "conn_state is null");
          } else {
            switch (conn_state) {
            case SP_CONNECTION_STATE_UNDEFINED:
            case SP_CONNECTION_STATE_LOGGED_OUT:
            case SP_CONNECTION_STATE_LOGGED_IN:
            case SP_CONNECTION_STATE_OFFLINE:
              break;
            case SP_CONNECTION_STATE_DISCONNECTED:
              log_warn("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_initialize", "Disconnected!");
              signalDisconnected();
              break;
            }
          }
          
          do {
            sp_session_process_events(sp, &next_timeout);
          } while (next_timeout == 0);
          
          pthread_mutex_unlock(&g_spotify_mutex);
          if (g_stop) break;
          pthread_mutex_lock(&g_notify_mutex);
	}

	log_debug("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_initialize", "Cleaning up.");
	sp_session_release(g_sess);

	if (nativeCacheFolder) (*env)->ReleaseStringUTFChars(env, cacheFolder, nativeCacheFolder);
	signalInitialized(0);
	return 0;
}

JNIEXPORT jint JNICALL Java_jahspotify_impl_JahSpotifyImpl_nativeLogin(JNIEnv *env, jobject obj, jstring username, jstring password, jstring blob,
		jboolean savePassword) {
	sp_error err;
	const char *nativePassword = NULL;
	const char *nativeUsername = NULL;
	const char *nativeBlob = NULL;

	if (!username && (!password || !blob)) {
		log_error("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_initialize", "Try to login without username and/or password.");
		err = sp_session_relogin(g_sess);

		if (err == SP_ERROR_NO_CREDENTIALS) {
			log_error("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_initialize", "Username or password not specified and not remembered.");
			return 1;
		}
	} else {
		nativeUsername = (*env)->GetStringUTFChars(env, username, NULL );
		if (password) nativePassword = (*env)->GetStringUTFChars(env, password, NULL );
		if (blob) nativeBlob = (*env)->GetStringUTFChars(env, blob, NULL );

		log_debug("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_initialize", "Locking");
		log_debug("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_initialize", "Initiating login: %s", nativeUsername);
		if (savePassword == JNI_TRUE) log_debug("jahspotify", "Java_jahspotify_impl_JahSpotifyImpl_initialize", "Going to remember this user.");
		sp_session_login(g_sess, nativeUsername, nativePassword, savePassword == JNI_TRUE ? 1 : 0, nativeBlob);
	}

	if (nativeUsername) (*env)->ReleaseStringUTFChars(env, username, nativeUsername);
	if (nativePassword) (*env)->ReleaseStringUTFChars(env, password, nativePassword);
	if (nativeBlob) (*env)->ReleaseStringUTFChars(env, blob, nativeBlob);

	return 0;
}

JNIEXPORT void JNICALL Java_jahspotify_impl_JahSpotifyImpl_nativeLogout(JNIEnv *env, jobject obj) {
  pthread_mutex_lock(&g_notify_mutex);
  sp_session_logout(g_sess);
  pthread_mutex_unlock(&g_notify_mutex);
}

JNIEXPORT void JNICALL Java_jahspotify_impl_JahSpotifyImpl_nativeForgetMe(JNIEnv *env, jobject obj) {
  pthread_mutex_lock(&g_notify_mutex);
  sp_session_forget_me(g_sess);
  pthread_mutex_unlock(&g_notify_mutex);
}

JNIEXPORT jint JNICALL Java_jahspotify_impl_JahSpotifyImpl_nativeDestroy(JNIEnv *env, jobject obj) {
  pthread_mutex_lock(&g_notify_mutex);
  g_stop_after_logout = 1;
  sp_session_logout(g_sess);
  pthread_mutex_unlock(&g_notify_mutex);
  return 0;
}


void addLoading(jobject javainstance, sp_track* track, sp_album* album, sp_artist* artist, int browse) {
  pthread_mutex_lock(&g_spotify_mutex);
  
  media *lmedia = malloc(sizeof *lmedia);
  lmedia->prev = NULL;
  lmedia->next = loading;
  lmedia->javainstance = javainstance;
  lmedia->track = track;
  lmedia->album = album;
  lmedia->artist = artist;
  lmedia->browse = browse;
  
  if (loading != NULL)
		loading->prev = lmedia;
  loading = lmedia;
  
  pthread_mutex_unlock(&g_spotify_mutex);
}

void checkLoaded() {
  pthread_mutex_lock(&g_spotify_mutex);
  
  JNIEnv* env = NULL;
  
  media *checkload = loading;
  while (checkload != NULL) {
    
    if (  (checkload->track && sp_track_is_loaded(checkload->track))
          || (checkload->artist && sp_artist_is_loaded(checkload->artist))
          || (checkload->album && sp_album_is_loaded(checkload->album))) {
      if (loading == checkload) { // First
        loading = checkload->next;
        if (loading != NULL)
          loading->prev = NULL;
      } else { // Any other.
        if (checkload->prev != NULL)
          checkload->prev->next = checkload->next;
        if (checkload->next != NULL)
          checkload->next->prev = checkload->prev;
      }
      
      if (!env && !retrieveEnv((JNIEnv*) &env)) return;
      
      if (checkload->track) {
        populateJTrackInstance(env, checkload->javainstance, checkload->track);
      } else if (checkload->artist) {
        populateJArtistInstance(env, checkload->javainstance, checkload->artist, checkload->browse);
      } else if (checkload->album) {
        populateJAlbumInstance(env, checkload->javainstance, checkload->album, checkload->browse);
      }
      (*env)->DeleteGlobalRef(env, checkload->javainstance);
      
      media *toFree = checkload;
      checkload = checkload->next;
      free(toFree);
    } else {
      checkload = checkload->next;
    }
  }
  if (env)
    detachThread();
  
  pthread_mutex_unlock(&g_spotify_mutex);
}
