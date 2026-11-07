// Bots de la sala pública (lobby_bots.cpp): mismo rango, prestigio e icono que tenían en la sala
// y la dificultad elegida en el menú de bots de la sala (lobby_bots_difficulty_allies / _enemies, -1 = la de la playlist)

main()
{
    // Menú de bots (cantidades por equipo): bots.gsc reparte los bots a partes iguales y echa a los que sobran
    // (EXE_PLAYERKICKED_BOT_BALANCE) en bot_connect_monitor; con cantidades manuales los bots son solo los de la sala
    // También en automático si la partida es de la sala con bots: bot_connect_monitor creaba sus propios bots (con
    // otro nombre y rango) antes que los de la sala y llenaba la partida
    if ( lobbybotsactive() || getdvarint( "lobby_bots_allies", -1 ) >= 0 || getdvarint( "lobby_bots_enemies", -1 ) >= 0 )
        replacefunc( scripts\mp\bots\bots::bot_connect_monitor, ::bot_connect_monitor_manual );

    // En partida pública el equipo de cada jugador lo pone el servidor (sessionteam, a partes iguales:
    // playerlogic -> scripts/mp/menus::setteam): los bots de la sala van al equipo que tenían en la sala
    // (los demás jugadores y bots, igual que siempre)
    replacefunc( scripts\mp\menus::setteam, ::setteam_lobby );

    // Rango y prestigio: en cada aparición rank.gsc pone a los bots un rango aleatorio según su dificultad
    // (bots::bot_get_rank_xp) y prestigio 0 (rank::playerupdaterank): los bots de la sala usan el de la sala
    replacefunc( scripts\mp\bots\bots::bot_get_rank_xp, ::bot_get_rank_xp_lobby );
    replacefunc( scripts\mp\rank::playerupdaterank, ::playerupdaterank_lobby );

    level thread on_player_connect();
    level thread match_finished_watch();
}

// Fin normal de la partida (lobby_bots.cpp): al volver a la sala se mantiene el equipo elegido con L2 / R2; si te
// sales antes, vuelve a automático. Al salirse el anfitrión también llega game_ended, pero el servidor se cierra en
// ese mismo momento: solo cuenta si la partida sigue un momento después (marcador final)
match_finished_watch()
{
    setdvar( "lobby_bots_match_finished", 0 );
    level waittill( "game_ended" );
    wait 1;
    setdvar( "lobby_bots_match_finished", 1 );
}

bot_connect_monitor_manual( num_ally_bots, num_enemy_bots )
{
}

// Copia de scripts/mp/menus::setteam (iw7) que cambia el equipo de los bots de la sala por el suyo
setteam_lobby( team )
{
    self endon( "disconnect" );

    if ( isbot( self ) )
    {
        lobby_team = lobbybotteam( self.name );
        if ( lobby_team != "" )
        {
            team = lobby_team;
            self.bot_team = lobby_team;
        }
    }

    if ( !isai( self ) && level.teambased && !scripts\mp\teams::getjointeampermissions( team ) && !scripts\mp\utility::lobbyteamselectenabled() )
        return;

    if ( level.ingraceperiod && !self.hasdonecombat )
    {
        self.hasspawned = 0;
        self.pers["lives"] = scripts\mp\utility::getgametypenumlives();
    }

    if ( self.sessionstate == "playing" )
    {
        self.switching_teams = 1;
        self.joining_team = team;
        self.leaving_team = self.pers["team"];
    }

    scripts\mp\menus::addtoteam( team );

    if ( scripts\mp\utility::isragdollzerog() )
        self lockdeathcamera( 1 );

    if ( self.sessionstate == "playing" )
        self suicide();

    scripts\mp\menus::waitforclassselect();
    scripts\mp\menus::endrespawnnotify();

    if ( self.sessionstate == "spectator" )
    {
        if ( game["state"] == "postgame" )
            return;

        if ( game["state"] == "playing" && !scripts\mp\utility::isinkillcam() )
        {
            if ( isdefined( self.waitingtospawnamortize ) && self.waitingtospawnamortize )
                return;

            thread scripts\mp\playerlogic::spawnclient();
        }

        thread scripts\mp\spectating::setspectatepermissions();
    }

    self notify( "okToSpawn" );
}

on_player_connect()
{
    for ( ;; )
    {
        level waittill( "connected", player );
        player thread apply_lobby_rank();

        if ( isbot( player ) )
            player thread apply_lobby_difficulty();
    }
}

apply_lobby_rank()
{
    self endon( "disconnect" );

    rank = lobbybotrank( self.name );
    if ( rank < 0 )
        return;

    prestige = lobbybotprestige( self.name );

    // rank.gsc y bots.gsc ponen el suyo al conectar y al aparecer: se vuelve a aplicar después
    set_lobby_rank( rank, prestige );
    waittillframeend;
    set_lobby_rank( rank, prestige );
    wait 0.5;
    set_lobby_rank( rank, prestige );
    self waittill( "spawned_player" );
    set_lobby_rank( rank, prestige );
}

set_lobby_rank( rank, prestige )
{
    self.pers["rankxp"] = scripts\mp\rank::getrankinfominxp( rank );
    self.pers["rank"] = rank;
    self.pers["prestige"] = prestige;
    self setrank( rank, prestige );
}

difficulty_name( value )
{
    switch ( value )
    {
        case 0:
            return "recruit";
        case 1:
            return "regular";
        case 2:
            return "hardened";
        case 3:
            return "veteran";
    }

    return undefined;
}

get_host_team()
{
    foreach ( player in level.players )
    {
        if ( !isbot( player ) && isdefined( player.team ) )
            return player.team;
    }

    return undefined;
}

apply_lobby_difficulty()
{
    self endon( "disconnect" );
    level endon( "game_ended" );

    for ( ;; )
    {
        self waittill( "spawned_player" );

        host_team = get_host_team();
        is_enemy = !level.teambased || !isdefined( host_team ) || self.team != host_team;

        if ( is_enemy )
            difficulty = difficulty_name( getdvarint( "lobby_bots_difficulty_enemies", -1 ) );
        else
            difficulty = difficulty_name( getdvarint( "lobby_bots_difficulty_allies", -1 ) );

        if ( !isdefined( difficulty ) )
            continue;

        // bots.gsc guarda la dificultad elegida (mezcla) en var_2D32 y la vuelve a poner: se quita
        self.var_2D32 = undefined;

        if ( difficulty != self botgetdifficulty() )
            self scripts\mp\bots\bots_util::bot_set_difficulty( difficulty );
    }
}

// Copia de scripts/mp/bots/bots::bot_get_rank_xp (iw7) con el rango de la sala para sus bots
bot_get_rank_xp_lobby()
{
    rank = lobbybotrank( self.name );
    if ( rank >= 0 )
    {
        self.pers["rankxp"] = scripts\mp\rank::getrankinfominxp( rank );
        return self.pers["rankxp"];
    }

    if ( scripts\mp\utility::bot_israndom() == 0 )
    {
        if ( !isdefined( self.pers["rankxp"] ) )
            self.pers["rankxp"] = 0;

        return self.pers["rankxp"];
    }

    difficulty = self botgetdifficulty();
    key = "bot_rank_" + difficulty;

    if ( isdefined( self.pers[key] ) && self.pers[key] > 0 )
        return self.pers[key];

    ranks = scripts\mp\bots\bots::bot_random_ranks_for_difficulty( difficulty );
    min_xp = scripts\mp\rank::getrankinfominxp( ranks["rank"] );
    max_xp = min_xp + scripts\mp\rank::getrankinfoxpamt( ranks["rank"] );
    xp = randomintrange( min_xp, max_xp + 1 );
    self.pers[key] = xp;
    return xp;
}

// Copia de scripts/mp/rank::playerupdaterank (iw7) con el prestigio de la sala para sus bots
playerupdaterank_lobby()
{
    if ( self.pers["rankxp"] < 0 )
        self.pers["rankxp"] = 0;

    rank = scripts\mp\rank::getrankforxp( scripts\mp\rank::getrankxp() );
    self.pers["rank"] = rank;

    if ( isai( self ) || !isdefined( self.pers["prestige"] ) )
    {
        prestige = 0;

        if ( isai( self ) && lobbybotrank( self.name ) >= 0 )
            prestige = lobbybotprestige( self.name );
        else if ( level.rankedmatch && isdefined( self.bufferedstats ) )
            prestige = scripts\mp\rank::getprestigelevel();

        self setrank( rank, prestige );
        self.pers["prestige"] = prestige;
    }

    if ( isdefined( self.clientid ) && self.clientid < level.maxlogclients )
    {
        setmatchdata( "players", self.clientid, "rank", rank );
        setmatchdata( "players", self.clientid, "Prestige", self.pers["prestige"] );
    }
}
