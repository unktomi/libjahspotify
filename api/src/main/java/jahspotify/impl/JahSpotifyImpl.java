package jahspotify.impl;

import jahspotify.Bitrate;
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
import jahspotify.services.JahSpotifyService;
import jahspotify.services.MediaHelper;

import java.awt.Graphics;
import java.awt.image.BufferedImage;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantLock;

import javax.imageio.ImageIO;

import org.apache.commons.logging.Log;
import org.apache.commons.logging.LogFactory;

/**
 * @author Johan Lindquist
 */
public class JahSpotifyImpl implements JahSpotify
{
	private PlayerStatus status = PlayerStatus.STOPPED;
    private static Log _log = LogFactory.getLog(JahSpotify.class);

    private Lock _libSpotifyLock = new ReentrantLock();

    private boolean _loggedIn = false;
    private boolean _loggingIn = false;
    private boolean _connected;
    private boolean initialized = false;
    private boolean playlistsLoadedBefore = false;

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
            public void playlist(final Playlist playlist)
            {
            	_log.trace(String.format("Playlist loaded: link=%s", playlist.getId()));
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
            public void artist(final int token, final Artist artist)
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
			public int addToBuffer(final byte[] buffer) {
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

                for (final ConnectionListener listener : _connectionListeners)
                {
                	new Thread() {
                		@Override
						public void run() {listener.playTokenLost();}
                	}.start();
                }
			}
        });

        registerNativeSearchCompleteListener(new NativeSearchCompleteListener()
        {
            @Override
            public void searchCompleted(final int token, final SearchResult searchResult)
            {
                _log.debug(String.format("Search completed: token=%d", token));

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
                for (final ConnectionListener listener : _connectionListeners)
                {
                	new Thread() {
                		@Override
						public void run() {listener.connected();}
                	}.start();
                }
            }

            @Override
            public void disconnected()
            {
                _log.debug("Disconnected");
                _connected = false;
            }

            @Override
            public void loggedIn(final boolean success)
            {
                _log.debug("Login result: " + success);
                _loggedIn = success;
                _connected = success;
                _loggingIn = false;
                for (final ConnectionListener listener : _connectionListeners)
                {
                	new Thread() {
                		@Override
						public void run() {listener.loggedIn(success);}
                	}.start();
                }
            }

            @Override
            public void loggedOut()
            {
                _log.debug("Logged out");
                _loggedIn = false;

                for (final ConnectionListener listener : _connectionListeners)
                {
                	new Thread() {
                		@Override
						public void run() {listener.loggedOut();}
                	}.start();
                }
            }

			@Override
			public void blobUpdated(final String blob) {
				for (final ConnectionListener listener : _connectionListeners)
                {
                	new Thread() {
                		@Override
						public void run() {listener.blobUpdated(blob);}
                	}.start();
                }
			}

			@Override
			public void initialized(final boolean initialized) {
				JahSpotifyImpl.this.initialized = initialized;
				for (final ConnectionListener listener : _connectionListeners)
                {
                	new Thread() {
                		@Override
						public void run() {listener.initialized(initialized);}
                	}.start();
                }
			}

			@Override
			public void playlistsLoaded() {
				if (playlistsLoadedBefore) return;
				playlistsLoadedBefore = true;
				boolean allLoaded = true;
				for (Playlist pl : PlaylistContainer.getPlaylists()) {
					if (!pl.isLoaded()) {
						allLoaded = false;
						break;
					}
				}

				final boolean contents = allLoaded;
				for (final ConnectionListener listener : _connectionListeners) {
                	new Thread() {
                		@Override
						public void run() {listener.playlistsLoaded(contents);}
                	}.start();
                }

				if (contents) {
					return;
				}

				// Keep waiting for the contents of the playlists in a new thread.
				new Thread() {
					@Override
					public void run() {
						for (Playlist pl : PlaylistContainer.getPlaylists()) {
							while (isLoggedIn() && !MediaHelper.waitFor(pl, 5))
								; // Do nothing.
						}
						for (final ConnectionListener listener : _connectionListeners) {
							new Thread() {
								@Override
								public void run() {listener.playlistsLoaded(true);}
							}.start();
						}
					}
				}.start();
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
            	nativeInitialize(cacheFolder);
            }
        };
        _jahSpotifyThread.start();
    }

    @Override
	public void destroy() {
    	_jahSpotifyThread = null;
    	nativeDestroy();
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
	public void logout() {
		_libSpotifyLock.lock();
    	try {
        	nativeLogout();
        } finally {
            _libSpotifyLock.unlock();
        }
	}

    @Override
	public void forgetMe() {
    	_libSpotifyLock.lock();
    	try {
        	nativeForgetMe();
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
    public Album readAlbum(final Link uri, final boolean browse)
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
    public Artist readArtist(final Link uri, final boolean browse)
    {
    	return readArtist(uri, browse ? 1 : 0);
    }
    /**
     * Reads the artist
     * @param uri The uri of the artist
     * @param browse 0 for no, 1 for yes, 2 for yes, but don't browse for tracks and albums.
     * @return
     */
    private Artist readArtist(final Link uri, final int browse) {
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
    public Track readTrack(final Link uri)
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

        if (uri.isPlaylistLink()) {
			try {
				return createPlaylistImage(uri);
			} catch (IOException e) {
				_log.warn("Unable to create playlist image.");
			}
        }

        uri = getCorrectImageLink(uri);
        if (uri == null) return null;

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
    /**
     * Returns the link for the image of the given linktype.
     * @param link
     * @return
     */
    private Link getCorrectImageLink(final Link link) {
    	switch (link.getType()) {
	    	case ALBUM:
				Album album = readAlbum(link);
				return album.getCover();
			case ARTIST:
				Artist artist = readArtist(link, 2);
				if (!MediaHelper.waitFor(artist, 2)) break;
				List<Link> links = artist.getPortraits();
				if (links.size() > 0) return links.get(0);

				// No artist image available. Get an album cover.
				artist = readArtist(link, 1);
				MediaHelper.waitFor(artist, 2);
				if (artist.getAlbums() == null || artist.getAlbums().size() == 0)
					return null;
				return readAlbum(artist.getAlbums().get(0)).getCover();
			case TRACK:
				Track t = readTrack(link);
				return JahSpotifyService.getInstance().getJahSpotify().readAlbum(t.getAlbum()).getCover();
			case IMAGE:
				return link;
			default: throw new RuntimeException("Unable to get an image from a " + link.getType() + " link ("+link+").");
    	}
    	return null;
    }

    /**
     * Gets the playlist image if available. If not this method will try to create a
     * 2x2 image of the albums of the first 4 tracks in the playlist.
     * If there are less than 4 different albums, only the first album will be used.
     * @param link
     * @return
     * @throws IOException
     */
    private Image createPlaylistImage(final Link link) throws IOException {
		if (!link.isPlaylistLink()) throw new IllegalArgumentException("Link should be of type playlist");
		Playlist playlist = readPlaylist(link, 0, 0);
		MediaHelper.waitFor(playlist, 1);

		// If the playlist has a custom image, use that.
		if (playlist.getPicture() != null)
			return readImage(playlist.getPicture());

		// Get the first 4 different images.
		Set<Link> albums = new TreeSet<Link>();
		for (int i = 0; i < playlist.getTracks().size(); i++) {
			Track track = readTrack(playlist.getTracks().get(i));
			if (albums.contains(track.getAlbum())) continue;
			albums.add(track.getAlbum());
			if (albums.size() == 4) break;
		}

		if (albums.size() == 0) return null; // Empty playlist, no image.
		if (albums.size() < 4) return readImage(albums.iterator().next()); // Too few images, just get the first one.

		// Create an image with the 4 images combined.
		List<Image> images = new ArrayList<Image>();
		for (Link iLink : albums) {
			images.add(readImage(iLink));
		}
		MediaHelper.waitFor(images, 2);

		// Make usable image from the Spotify image types.
		List<BufferedImage> bImages = new ArrayList<BufferedImage>();
		Image correct = null;
		for (Image image : images) {
			if (image.getBytes() != null) {
				correct = image;
				bImages.add(ImageIO.read(new ByteArrayInputStream(image.getBytes())));
			}
		}
		if (bImages.size() != 4) return correct;

		// Draw the target image.
		BufferedImage target = new BufferedImage(300, 300, BufferedImage.TYPE_INT_RGB);
		Graphics g = target.getGraphics();
		BufferedImage image;
		image = bImages.remove(0); g.drawImage(image,   0,   0, 150, 150, 0, 0, image.getWidth(), image.getHeight(), null);
		image = bImages.remove(0); g.drawImage(image, 150,   0, 300, 150, 0, 0, image.getWidth(), image.getHeight(), null);
		image = bImages.remove(0); g.drawImage(image,   0, 150, 150, 300, 0, 0, image.getWidth(), image.getHeight(), null);
		image = bImages.remove(0); g.drawImage(image, 150, 150, 300, 300, 0, 0, image.getWidth(), image.getHeight(), null);

		ByteArrayOutputStream baos = new ByteArrayOutputStream();
		ImageIO.write(target, "JPG", baos);

		Image result = new Image();
		result.setBytes(baos.toByteArray());
		result.setLoaded(true);

		return result;
	}

    @Override
    public Playlist readPlaylist(final Link uri, final int index, final int numEntries)
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
	public SearchResult getTopList(final TopListType type) {
    	return getTopList(type, null);
    }
    @Override
	public SearchResult getTopList(final TopListType type, final String country) {
    	int countrycode = -1;
    	if (country != null && country.length() == 2) {
    		countrycode = country.charAt(0) << 8 | country.charAt(1);
    	}
    	ensureLoggedIn();
    	_libSpotifyLock.lock();
    	try {
    		return retrieveTopList(type.ordinal(), countrycode);
    	} finally {
    		_libSpotifyLock.unlock();
    	}
    }

    private Playlist trimPlaylist(final Playlist playlist, final int index, int numEntries)
    {
        Playlist trimmedPlaylist = new Playlist();
        trimmedPlaylist.setAuthor(playlist.getAuthor());
        trimmedPlaylist.setCollaborative(playlist.isCollaborative());
        trimmedPlaylist.setDescription(playlist.getDescription());
        trimmedPlaylist.setId(playlist.getId());
        trimmedPlaylist.setName(playlist.getName());
        trimmedPlaylist.setPicture(playlist.getPicture());
        numEntries = Math.min(numEntries, playlist.getNumTracks());
        trimmedPlaylist.setNumTracks(numEntries == 0 ? playlist.getNumTracks() : numEntries);
        trimmedPlaylist.setIndex(index);
        // FIXME: Trim this list
        trimmedPlaylist.setTracks(playlist.getTracks().subList(index, numEntries-index));
        return null;
    }

    @Override
    public void pause()
    {
        ensureLoggedIn();
        nativePause();
        status = PlayerStatus.PAUSED;
    }

    private native int nativePause();

    @Override
    public void resume()
    {
        ensureLoggedIn();
        nativeResume();
        status = PlayerStatus.PLAYING;
    }

	@Override
	public void setBitrate(final Bitrate rate) {
		if (!initialized)
			throw new RuntimeException("libJah'Spotify isn't initialized yet.");
		setBitrate(rate.ordinal());
	}

    private native int nativeResume();

    @Override
    public void play(final Link link)
    {
        ensureLoggedIn();
        nativePlayTrack(link.asString());
        status = PlayerStatus.PLAYING;
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
    	if (_loggedIn) _loggingIn = false;
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
    	try {
    		String nativeLibrary = System.getProperty("jahspotify.lib", null);
    		if(nativeLibrary != null) {
    			System.load(nativeLibrary);
    		} else {
	    		// The native-jar is an optional dependency. Use it when it is available.
				Class<?> loader = Class.forName("jahspotify.JahSpotifyNativeLoader");
				loader.newInstance();
    		}
		} catch (Exception e) {
			_log.warn("The native-jar was not found or could not load the required libraries. Trying to load jahspotify without it.");
			System.loadLibrary("jahspotify");
		}
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
    	if (initialized)
    		connectionListener.initialized(true);
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
        status = PlayerStatus.STOPPED;
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

    public NativeSearchParameters initializeFromSearch(final Search search)
    {
        NativeSearchParameters nativeSearchParameters = new NativeSearchParameters();
        nativeSearchParameters._query = search.getQuery().serialize();
        nativeSearchParameters.albumOffset = search.getAlbumOffset();
        nativeSearchParameters.artistOffset = search.getArtistOffset();
        nativeSearchParameters.trackOffset = search.getTrackOffset();
        nativeSearchParameters.playlistOffset = search.getPlaylistOffset();
        nativeSearchParameters.numAlbums = search.getNumAlbums();
        nativeSearchParameters.numArtists = search.getNumArtists();
        nativeSearchParameters.numTracks = search.getNumTracks();
        nativeSearchParameters.numPlaylists = search.getNumPlaylists();
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

        int playlistOffset = 0;
        int numPlaylists = 255;
    }
    
    @Override
    public PlayerStatus getStatus() {
    	return status;
    }

    private native int nativeInitialize(String cacheFolder);
    private native int nativeDestroy();
	private native int nativeLogin(String username, String password, String blob, boolean savePassword);
	private native void nativeLogout();
	private native void nativeForgetMe();

    private native boolean registerNativeMediaLoadedListener(final NativeMediaLoadedListener nativeMediaLoadedListener);

    private native void readImage(String uri, Image image);

    private native User retrieveUser();

    private native Album retrieveAlbum(String uri, boolean browse);

    private native Artist retrieveArtist(String uri, int browse);

    private native Track retrieveTrack(String uri);

    private native Playlist retrievePlaylist(String uri);
    private native SearchResult retrieveTopList(int type, int countrycode);

    private native void setBitrate(int bitrate);
    private native int nativePlayTrack(String uri);
    private native void nativeStopTrack();
    private native void nativeTrackSeek(int offset);

    private native void nativeInitiateSearch(final int i, NativeSearchParameters token);
    private native boolean registerNativeConnectionListener(final NativeConnectionListener nativeConnectionListener);
    private native boolean registerNativeSearchCompleteListener(final NativeSearchCompleteListener nativeSearchCompleteListener);

    private native boolean nativeShutdown();

    private native boolean registerNativePlaybackListener(NativePlaybackListener playbackListener);

}
