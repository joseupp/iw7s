// Círculo de ganadores: mientras dura (level.broshotrunning, scripts\mp\broshot.gsc) pone cg_broshotActive para que el
// cliente (fov.cpp) use FOV 65 sin escalas en la cámara del círculo.

main()
{
    setdvar( "cg_broshotActive", 0 );
    level thread watch_broshot();
}

watch_broshot()
{
    active = 0;

    for (;;)
    {
        running = isdefined( level.broshotrunning ) && level.broshotrunning;

        if ( running != active )
        {
            active = running;
            setdvar( "cg_broshotActive", active );
        }

        wait 0.05;
    }
}
