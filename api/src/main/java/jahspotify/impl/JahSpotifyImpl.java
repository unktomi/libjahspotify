package jahspotify.impl;

import jahspotify.ConnectionListener;
import jahspotify.JahSpotify;
import jahspotify.PlaybackListener;
import jahspotify.PlaylistListener;
import jahspotify.Search;
import jahspotify.SearchListener;
import jahspotify.SearchResult;
import jahspotify.media.Album;
import jahspotify.media.Artist;
import jahspotify.media.Image;
import jahspotify.media.ImageSize;
import jahspotify.media.Link;
import jahspotify.media.Playlist;
import jahspotify.media.PlaylistContainer;
import jahspotify.media.TopListType;
import jahspotify.media.Track;
import jahspotify.media.User;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantLock;

import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;

/**
 * @author Johan Lindquist
 */
public class JahSpotifyImpl implements JahSpotify
{
    private Log _log = LogFactory.getLog(JahSpotify.class);

    private Lock _libSpotifyLock = new ReentrantLock();

    private boolean _loggedIn = false;
    private boolean _loggingIn = false;
    private boolean _connected;
    private boolean initialized = false;

    private List<PlaybackListener> _playbackListeners = new ArrayList<PlaybackListener>();
    private List<ConnectionListener> _connectionListeners = new ArrayList<ConnectionListener>();

    private List<SearchListener> _searchListeners = new ArrayList<SearchListener>();
    private Map<Integer, SearchListener> _prioritySearchListeners = new HashMap<Integer, SearchListener>();
    private List<PlaylistListener> _playlistListeners = new ArrayList<PlaylistListener>();

    private Thread _jahSpotifyThread;
    private static JahSpotifyImpl _jahSpotify;
    private boolean _synching = false;
    private User _user;
    private AtomicInteger _globalToken = new AtomicInteger(1);

    private final PlaylistContainer playlistContainer = new PlaylistContainer();

    protected JahSpotifyImpl()
    {
        registerNativeMediaLoadedListener(new NativeMediaLoadedListener()
        {
            @Override
            public void track(final int token, final Link link)
            {
                _log.trace(String.format("Track loaded: token=%d link=%s", token, link));
            }

            @Override
            public void playlist(final int token, final Link link, final String name)
            {
            	playlistContainer.addPlaylist(link, name);
            }

            @Override
            public void album(final int token, final Album album)
            {
                albumLoadedCallback(token, album);
            }

            @Override
            public void image(final int token, final Link link, final ImageSize imageSize, final byte[] imageBytes)
            {
                imageLoadedCallback(token, link,imageSize,imageBytes);
            }

            @Override
            public void artist(final int token, Artist artist)
            {
                artistLoadedCallback(token, artist);
            }
        });

        registerNativePlaybackListener(new NativePlaybackListener()
        {
            @Override
            public void trackStarted(final String uri)
            {
                _log.debug("Track started: " + uri);
                for (PlaybackListener listener : _playbackListeners)
                {
                    listener.trackStarted(Link.create(uri));
                }
            }

            @Override
            public void trackEnded(final String uri, final boolean forcedEnd)
            {
                _log.debug("Track ended signalled: " + uri + " (" + (forcedEnd ? "forced)" : "natural ending)"));
                for (PlaybackListener listener : _playbackListeners)
                {
                    listener.trackEnded(Link.create(uri), forcedEnd);
                }

            }

            @Override
            public String nextTrackToPreload()
            {
                _log.debug("Next to pre-load, will query listeners");
                for (PlaybackListener listener : _playbackListeners)
                {
                    Link nextTrack = listener.nextTrackToPreload();
                    if (nextTrack != null)
                    {
                        _log.debug("Listener returned non-null value: " + nextTrack);
                        return nextTrack.asString();
                    }
                }
                return null;
            }

			@Override
			public void setAudioFormat(final int rate, final int channels) {
                for (PlaybackListener listener : _playbackListeners)
                {
                	listener.setAudioFormat(rate, channels);
                }
			}

			@Override
			public int addToBuffer(byte[] buffer) {
				int highestReturn = 0;
				for (PlaybackListener listener : _playbackListeners)
                {
					highestReturn = Math.max(listener.addToBuffer(buffer), highestReturn);
                }
				return highestReturn;
			}

			@Override
			public void playTokenLost() {
                for (PlaybackListener listener : _playbackListeners)
                {
                    listener.playTokenLost();
                }
			}
        });

        registerNativeSearchCompleteListener(new NativeSearchCompleteListener()
        {
            @Override
            public void searchCompleted(final int token, final SearchResult searchResult)
            {
                _log.debug(String.format("Search completed: token=%d searchResult=%s", token, searchResult));

                if (token > 0)
                {
                    final SearchListener searchListener = _prioritySearchListeners.get(token);
                    if (searchListener != null)
                    {
                        searchListener.searchComplete(searchResult);
                    }
                }
                for (SearchListener searchListener : _searchListeners)
                {
                    searchListener.searchComplete(searchResult);
                }
            }
        });

        registerNativeConnectionListener(new NativeConnectionListener()
        {
            @Override
            public void connected()
            {
                _connected = true;
                for (ConnectionListener listener : _connectionListeners)
                {
                    listener.connected();
                }
            }

            @Override
            public void disconnected()
            {
                _log.debug("Disconnected");
                _connected = false;
                _loggedIn = false;
            }

            @Override
            public void loggedIn(boolean success)
            {
                _log.debug("Login result: " + success);
                _loggedIn = success;
                _connected = success;
                _loggingIn = false;
                if (!success) return;
                for (ConnectionListener listener : _connectionListeners)
                {
                    listener.loggedIn();
                }
            }

            @Override
            public void loggedOut()
            {
                _log.debug("Logged out");
                _loggedIn = false;
            }

			@Override
			public void blobUpdated(String blob) {
				for (ConnectionListener listener : _connectionListeners)
                    listener.blobUpdated(blob);
			}
        });
    }

    @Override
	public synchronized void initialize(final String cacheFolder) {
        if (_jahSpotifyThread != null)
            return;

        _jahSpotifyThread = new Thread("libJahSpotify native message handler")
        {
            @Override
            public void run()
            {
            	initialized = true;
            	nativeInitialize(cacheFolder);
            	initialized = false;
            }
        };
        _jahSpotifyThread.start();
    }

    @Override
	public void destroy() {
    	_jahSpotifyThread = null;
    	nativeDestroy();
	}

    @Override
	public PlaylistContainer getPlaylistContainer() {
		return playlistContainer;
	}

    protected void albumLoadedCallback(final int token, final Album album)
    {
        _log.trace(String.format("Album loaded: token=%d link=%s", token, album.getId()));
    }

    protected void imageLoadedCallback(final int token, final Link link, final ImageSize imageSize, final byte[] imageBytes)
    {
        _log.trace(String.format("Image loaded: token=%d link=%s", token, link));
    }

    protected void artistLoadedCallback(final int token, final Artist artist)
    {
        _log.trace(String.format("Artist loaded: token=%d link=%s", token, artist.getId()));
    }

    public static synchronized JahSpotify getInstance()
    {
        if (_jahSpotify == null)
            _jahSpotify = new JahSpotifyImpl();
        return _jahSpotify;
    }

    @Override
    public void login(final String username, final String password, final String blob, final boolean savePassword)
    {
    	if (!initialized)
    		throw new IllegalStateException("You should initialize libJah'Spotify before attempting to login.");
    	_loggingIn = false;
    	if (_loggingIn) return; // Still trying to login.
        _libSpotifyLock.lock();
        try {
        	_loggingIn = true;
        	nativeLogin(username, password, blob, savePassword);
        } finally {
            _libSpotifyLock.unlock();
        }
    }

    @Override
    public Album readAlbum(final Link uri)
    {
        return readAlbum(uri, false);
    }
    @Override
    public Album readAlbum(final Link uri, boolean browse)
    {
        ensureLoggedIn();

        _libSpotifyLock.lock();
        try
        {
            return retrieveAlbum(uri.asString(), browse);
        }
        finally
        {
            _libSpotifyLock.unlock();
        }
    }

    @Override
    public Artist readArtist(final Link uri)
    {
    	return readArtist(uri, false);
    }
    @Override
    public Artist readArtist(final Link uri, boolean browse)
    {
        ensureLoggedIn();

        _libSpotifyLock.lock();
        try
        {
            return retrieveArtist(uri.asString(), browse);
        }
        finally
        {
            _libSpotifyLock.unlock();
        }
    }


    @Override
    public Track readTrack(Link uri)
    {
        ensureLoggedIn();
        _libSpotifyLock.lock();
        try
        {
            return retrieveTrack(uri.asString());
        }
        finally
        {
            _libSpotifyLock.unlock();
        }
    }

    @Override
    public Image readImage(Link uri)
    {
        ensureLoggedIn();

        _libSpotifyLock.lock();
        Image image = new Image(uri);
        try
        {
            readImage(uri.getId(), image);
        }
        finally
        {
            _libSpotifyLock.unlock();
        }

        return image;
    }

    @Override
    public Playlist readPlaylist(Link uri, final int index, final int numEntries)
    {
        ensureLoggedIn();
        _libSpotifyLock.lock();
        try
        {
            final Playlist playlist = retrievePlaylist(uri == null ? null : uri.asString());
            if (index == 0 && numEntries == 0 || playlist == null)
                return playlist;

            // Trim the playlist accordingly now
            return trimPlaylist(playlist, index, numEntries);
        }
        finally
        {
            _libSpotifyLock.unlock();
        }
    }

    @Override
	public SearchResult getTopList(TopListType type) {
    	ensureLoggedIn();
    	_libSpotifyLock.lock();
    	try {
    		return retrieveTopList(type.ordinal());
    	} finally {
    		_libSpotifyLock.unlock();
    	}
    }

    private Playlist trimPlaylist(final Playlist playlist, final int index, final int numEntries)
    {
        Playlist trimmedPlaylist = new Playlist();
        trimmedPlaylist.setAuthor(playlist.getAuthor());
        trimmedPlaylist.setCollaborative(playlist.isCollaborative());
        trimmedPlaylist.setDescription(playlist.getDescription());
        trimmedPlaylist.setId(playlist.getId());
        trimmedPlaylist.setName(playlist.getName());
        trimmedPlaylist.setPicture(playlist.getPicture());
        trimmedPlaylist.setNumTracks(numEntries == 0 ? playlist.getNumTracks() : numEntries);
        trimmedPlaylist.setIndex(index);
        // FIXME: Trim this list
        trimmedPlaylist.setTracks(playlist.getTracks().subList(index, numEntries));
        return null;
    }

    @Override
    public void pause()
    {
        ensureLoggedIn();
        nativePause();
    }

    private native int nativePause();

    @Override
    public void resume()
    {
        ensureLoggedIn();
        nativeResume();
    }

    private native int nativeResume();

    @Override
    public void play(Link link)
    {
        ensureLoggedIn();
        nativePlayTrack(link.asString());
    }

    private void ensureLoggedIn()
    {
        if (!_loggedIn)
        {
            throw new IllegalStateException("Not logged in");
        }
    }

    @Override
	public boolean isLoggedIn() {
		return _loggedIn;
	}

    @Override
	public boolean isLoggingIn() {
    	return _loggingIn;
    }

    @Override
    public User getUser()
    {
        ensureLoggedIn();

        if (_user != null)
        {
            return _user;
        }

        _user = retrieveUser();

        return _user;
    }

    static
    {
        System.loadLibrary("jahspotify");
    }

    @Override
    public void addPlaybackListener(final PlaybackListener playbackListener)
    {
        _playbackListeners.add(playbackListener);
    }

    @Override
    public void addPlaylistListener(final PlaylistListener playlistListener)
    {
        _playlistListeners.add(playlistListener);
    }

    @Override
    public void addConnectionListener(final ConnectionListener connectionListener)
    {
        _connectionListeners.add(connectionListener);
    }

    @Override
    public void addSearchListener(final SearchListener searchListener)
    {
        _searchListeners.add(searchListener);
    }

    @Override
    public void seek(final int offset)
    {
        ensureLoggedIn();
        nativeTrackSeek(offset);
    }

    @Override
    public void shutdown()
    {
        ensureLoggedIn();
        nativeShutdown();
    }

    @Override
    public boolean isStarted()
    {
        return _jahSpotifyThread != null;
    }

    @Override
    public void stop()
    {
        ensureLoggedIn();
        nativeStopTrack();
    }

    public void initiateSearch(final Search search)
    {
        ensureLoggedIn();

        _libSpotifyLock.lock();
        try
        {
            NativeSearchParameters nativeSearchParameters = initializeFromSearch(search);
            // TODO: Register the lister for the specified token
            nativeInitiateSearch(0, nativeSearchParameters);
        }
        finally
        {
            _libSpotifyLock.unlock();
        }
    }

    @Override
	public void initiateSearch(final Search search, final SearchListener searchListener)
    {
        ensureLoggedIn();

        _libSpotifyLock.lock();
        try
        {
            int token = _globalToken.getAndIncrement();
            NativeSearchParameters nativeSearchParameters = initializeFromSearch(search);
            _prioritySearchListeners.put(token, searchListener);
            nativeInitiateSearch(token, nativeSearchParameters);
        }
        finally
        {
            _libSpotifyLock.unlock();
        }
    }

    public NativeSearchParameters initializeFromSearch(Search search)
    {
        NativeSearchParameters nativeSearchParameters = new NativeSearchParameters();
        nativeSearchParameters._query = search.getQuery().serialize();
        nativeSearchParameters.albumOffset = search.getAlbumOffset();
        nativeSearchParameters.artistOffset = search.getArtistOffset();
        nativeSearchParameters.trackOffset = search.getTrackOffset();
        nativeSearchParameters.numAlbums = search.getNumAlbums();
        nativeSearchParameters.numArtists = search.getNumArtists();
        nativeSearchParameters.numTracks = search.getNumTracks();
        nativeSearchParameters.suggest = search.isSuggest();
        return nativeSearchParameters;
    }

    public static class NativeSearchParameters
    {
        String _query;
        boolean suggest;

        int trackOffset = 0;
        int numTracks = 255;

        int albumOffset = 0;
        int numAlbums = 255;

        int artistOffset = 0;
        int numArtists = 255;
    }

    private native int nativeInitialize(String cacheFolder);
    private native int nativeDestroy();
	private native int nativeLogin(String username, String password, String blob, boolean savePassword);

    private native boolean registerNativeMediaLoadedListener(final NativeMediaLoadedListener nativeMediaLoadedListener);

    private native void readImage(String uri, Image image);

    private native User retrieveUser();

    private native Album retrieveAlbum(String uri, boolean browse);

    private native Artist retrieveArtist(String uri, boolean browse);

    private native Track retrieveTrack(String uri);

    private native Playlist retrievePlaylist(String uri);
    private native SearchResult retrieveTopList(int type);

    private native int nativePlayTrack(String uri);
    private native void nativeStopTrack();
    private native void nativeTrackSeek(int offset);

    private native void nativeInitiateSearch(final int i, NativeSearchParameters token);
    private native boolean registerNativeConnectionListener(final NativeConnectionListener nativeConnectionListener);
    private native boolean registerNativeSearchCompleteListener(final NativeSearchCompleteListener nativeSearchCompleteListener);

    private native boolean nativeShutdown();

    private native boolean registerNativePlaybackListener(NativePlaybackListener playbackListener);

}
