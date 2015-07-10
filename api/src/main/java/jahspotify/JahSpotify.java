package jahspotify;

import jahspotify.media.Album;
import jahspotify.media.Artist;
import jahspotify.media.Image;
import jahspotify.media.Link;
import jahspotify.media.Playlist;
import jahspotify.media.TopListType;
import jahspotify.media.Track;
import jahspotify.media.User;

/**
 * Main interface into the Spotify system. This provides the API's required to
 * login and interact with the Spotify APIs.
 * 
 * @author Johan Lindquist
 */
public interface JahSpotify {
	public enum PlayerStatus {
		STOPPED,
		PLAYING,
		PAUSED;
	};
	/**
	 * Initializes libJahSpotify.
	 * 
	 * @param cacheFolder
	 */
	public void initialize(String cacheFolder);

	/**
	 * Stops libJahSpotify.
	 */
	public void destroy();

	/**
	 * Logs into the Spotify system. The specified account must be a Spotify
	 * premium account. <br/>
	 * Notifications of successful login will come via the
	 * {@link ConnectionListener} API.
	 * 
	 * @param username
	 *            Username to use for login.
	 * @param password
	 *            The password for the specified username.
	 */
	public void login(String username, String password, String blob,
			boolean savePassword);

	/**
	 * Logout the current user.
	 */
	public void logout();

	/**
	 * Forgets the currently remembered user.
	 */
	public void forgetMe();

	/**
	 * Read the information for the specified artist.
	 * 
	 * @param link
	 *            The link for the artist in question
	 * @return The read artist or null if it could not be read
	 */
	public Artist readArtist(Link link);

	public Artist readArtist(Link link, boolean browse);

	/**
	 * Read the information for the specified track.
	 * 
	 * @param link
	 *            The link for the track in question
	 * @return The read and loaded track or null if it could not be read.
	 */
	public Track readTrack(Link link);

	/**
	 * Read the information for the specified album.
	 * 
	 * @param link
	 *            The link for the album in question
	 * @return The read album or null if it could not be read
	 */
	public Album readAlbum(Link link, boolean browse);

	public Album readAlbum(Link link);

	/**
	 * Read the specified image.
	 * 
	 * @param link
	 *            The link for the image in question
	 * @return The read image or null if it could not be read
	 */
	public Image readImage(Link link);

	/**
	 * Read the information for the specified playlist.
	 * 
	 * @param link
	 *            The link for the playlist in question
	 * @param index
	 *            Index to start from
	 * @param numEntries
	 *            The number of entries of the playlist to retrieve @return The
	 *            read playlist or null if it could not be read
	 * @return The processed playlist
	 */
	public Playlist readPlaylist(Link link, final int index,
                                     final int numEntries);

	/**
	 * Retrieves a toplist.
	 * 
	 * @param Type
	 *            of toplist.
	 * @return SearchResult with the correct list filled.
	 */
	public SearchResult getTopList(TopListType type);

	/**
	 * Retrieves a toplist.
	 * 
	 * @param type
	 *            Type of playlist
	 * @param country
	 *            Country code, null for global, "SE" for sweden, "NL" for
	 *            netherlands etc.
	 * @return SearchResult with the correct list filled.
	 */
	public SearchResult getTopList(TopListType type, String country);

	/**
	 * Sets the bitrate for the tracks.
	 * 
	 * @param rate
	 */
	public void setBitrate(Bitrate rate);

	/**
	 * Pauses the currently playing track. Does nothing if nothing is currently
	 * playing
	 * 
	 */
	public void pause();

	/**
	 * Resumes the currently playing track. Does nothing if there is currently
	 * no track playing.
	 * 
	 */
	public void resume();

	/**
	 * Starts playback of the specified media link. This link may a track, an
	 * album, a playlist or a playlist folder.
	 * 
	 * @param link
	 *            The link of the media to play
	 */
	public void play(Link link);

	/**
	 * Retrieves information relating to the currently logged in user.
	 * 
	 * @return The currently logged in user or null if the user information can
	 *         not be read.
	 */
	public User getUser();

	/**
	 * Returns true iff a user is logged in.
	 * 
	 * @return
	 */
	public boolean isLoggedIn();

	/**
	 * Returns true iff an attempt to login is in progress.
	 * 
	 * @return
	 */
	public boolean isLoggingIn();

	/**
	 * Returns a flag reflecting the current state of the system, whether it is
	 * started or not.
	 * 
	 * @return True if the system is started and connected to Spotify, otherwise
	 *         false.
	 */
	public boolean isStarted();

	/**
	 * Shuts down the Spotify connection and cleans up any internal structures.
	 */
	public void stop();

	/**
	 * Seeks to the specied offset in the currently playing track
	 * 
	 * @param offset
	 *            The offset to seek to, in seconds
	 */
	public void seek(int offset);

	/**
	 * Initiates a search for the specified query. Results are returned
	 * asynchronously via the {@link SearchListener} API.
	 * 
	 * @param search
	 *            The search to execute towards the Spotify APIs. This bundles
	 *            both the query and result parameters.
	 * @param searchListener
	 *            The listener to report results found to
	 */
	public void initiateSearch(final Search search,
			final SearchListener searchListener);

	/**
	 * 
	 * @param playbackListener
	 */
	public void addPlaybackListener(PlaybackListener playbackListener);

	/**
	 * 
	 * @param playlistListener
	 */
	public void addPlaylistListener(PlaylistListener playlistListener);

	/**
	 * 
	 * @param connectionListener
	 */
	public void addConnectionListener(ConnectionListener connectionListener);

	/**
	 * 
	 * @param searchListener
	 */
	public void addSearchListener(SearchListener searchListener);

	/**
	 * Shuts down the JahSpotify instance.
	 */
	public void shutdown();
	
	public PlayerStatus getStatus(); 

}
