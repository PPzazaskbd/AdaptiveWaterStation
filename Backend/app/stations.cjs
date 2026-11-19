const {dateRange,page} = require('./time.cjs');
const columns = 'd.dispenser_id AS station_id, d.name, d.location, d.tank_capacity_ml, s.operation_status, s.remaining_water_ml, s.water_updated_at, s.last_seen_at';
const object = value => value !== null && typeof value==='object'&&!Array.isArray(value);
module.exports = (pool,json) => async (req,res,url,readBody) => {
  const route=url.pathname;
  if(route==='/api/stations'&&req.method==='GET') {
    const paging=page(url);
    const result=await pool.query(`SELECT ${columns} FROM dispenser d LEFT JOIN system_status s USING(dispenser_id) ORDER BY d.dispenser_id LIMIT $1 OFFSET $2`,[paging.limit,paging.offset]);
    json(res,200,{stations:result.rows,...paging});return true;
  }
  const match=/^\/api\/stations\/(\d+)(?:\/(dashboard|analytics|tank|fill-sessions|refills))?$/.exec(route);
  const global=route==='/api/dashboard';
  if(!match&&!global)return false;
  const id=match?match[1]:null;
  if(id&&(id.length>10||BigInt(id)<1n||BigInt(id)>2147483647n)){json(res,400,{error:'invalid_station_id'});return true;}
  let station;
  if(id){
    const result=await pool.query(`SELECT ${columns} FROM dispenser d LEFT JOIN system_status s USING(dispenser_id) WHERE d.dispenser_id=$1`,[id]);
    station=result.rows[0];if(!station){json(res,404,{error:'station_not_found'});return true;}
  }
  const section=global?'dashboard':match[2];
  if(id&&!section&&req.method==='PATCH') {
    if((req.headers['content-type']||'').split(';')[0].trim().toLowerCase()!=='application/json'){req.resume();json(res,415,{error:'use_application_json'});return true;}
    const changes=await readBody(req);
    if(!object(changes)||!Object.keys(changes).length||Object.keys(changes).some(k=>!['name','location'].includes(k))||
      ('name'in changes&&(typeof changes.name!=='string'||!changes.name.trim()||changes.name.length>200))||
      ('location'in changes&&changes.location!==null&&(typeof changes.location!=='string'||changes.location.length>500))){
      json(res,400,{error:'only_name_and_location_can_be_updated'});return true;
    }
    const result=await pool.query(`UPDATE dispenser SET name=CASE WHEN $2 THEN $3 ELSE name END,
      location=CASE WHEN $4 THEN $5 ELSE location END WHERE dispenser_id=$1
      RETURNING dispenser_id AS station_id,name,location,tank_capacity_ml`,
      [id,'name'in changes,changes.name?.trim()??null,'location'in changes,changes.location??null]);
    if(!result.rows.length){json(res,404,{error:'station_not_found'});return true;}
    json(res,200,result.rows[0]);return true;
  }
  if(req.method!=='GET'){
    req.resume();res.setHeader('Allow',id&&!section?'GET, PATCH':'GET');
    json(res,405,{error:'method_not_allowed'});return true;
  }
  if(id&&!section){json(res,200,station);return true;}
  if(section==='tank'){json(res,200,{station_id:station.station_id,tank_capacity_ml:station.tank_capacity_ml,
    remaining_water_ml:station.remaining_water_ml,water_updated_at:station.water_updated_at});return true;}
  const range=dateRange(url),args=[id,range.from,range.to_exclusive];
  if(section==='dashboard'||section==='analytics'){
    const totals=await pool.query(`SELECT count(*) AS completed_cycle_count,
      count(*) FILTER(WHERE volume_ml IS NULL) AS unknown_volume_cycle_count,
      COALESCE(sum(volume_ml),0) AS recorded_volume_ml FROM fill_session
      WHERE ended_at IS NOT NULL AND ($1::integer IS NULL OR dispenser_id=$1)
      AND ($2::timestamptz IS NULL OR ended_at >= $2) AND ($3::timestamptz IS NULL OR ended_at < $3)`,args);
    const daily=await pool.query(`SELECT (ended_at AT TIME ZONE 'Asia/Bangkok')::date::text AS day,
      count(*) AS completed_cycle_count,count(*) FILTER(WHERE volume_ml IS NULL) AS unknown_volume_cycle_count,
      COALESCE(sum(volume_ml),0) AS recorded_volume_ml FROM fill_session
      WHERE ended_at IS NOT NULL AND ($1::integer IS NULL OR dispenser_id=$1)
      AND ($2::timestamptz IS NULL OR ended_at >= $2) AND ($3::timestamptz IS NULL OR ended_at < $3)
      GROUP BY 1 ORDER BY 1 DESC LIMIT 366`,args);
    let current;
    if(global){
      const states=await pool.query(`SELECT count(*) AS station_count,
        count(*) FILTER(WHERE s.operation_status='DISPENSING') AS dispensing_count,
        count(*) FILTER(WHERE s.operation_status='STANDBY') AS standby_count,
        count(*) FILTER(WHERE s.operation_status IS NULL) AS unknown_operation_count
        FROM dispenser d LEFT JOIN system_status s USING(dispenser_id)`);
      current=states.rows[0];
    }
    json(res,200,{...(global?{current_state:current}:{station}),range,usage:totals.rows[0],daily:daily.rows,
      daily_limit:366,daily_order:'newest_first',notes:{cycles_are_not_confirmed_full_bottles:true,recorded_volume_excludes_unknown_volumes:true,current_state_is_not_date_filtered:true}});return true;
  }
  const paging=page(url);
  const refill=section==='refills';
  const sql=refill?`SELECT refill_id,dispenser_id AS station_id,refilled_at,added_volume_ml FROM tank_refill
    WHERE dispenser_id=$1 AND ($2::timestamptz IS NULL OR refilled_at >= $2) AND ($3::timestamptz IS NULL OR refilled_at < $3)
    ORDER BY refilled_at DESC,refill_id DESC LIMIT $4 OFFSET $5`:
    `SELECT fill_id,dispenser_id AS station_id,started_at,ended_at,volume_ml FROM fill_session
    WHERE dispenser_id=$1 AND ($2::timestamptz IS NULL OR started_at >= $2) AND ($3::timestamptz IS NULL OR started_at < $3)
    ORDER BY started_at DESC,fill_id DESC LIMIT $4 OFFSET $5`;
  const result=await pool.query(sql,[...args,paging.limit,paging.offset]);
  json(res,200,{[refill?'refills':'fill_sessions']:result.rows,range,...paging,filtered_by:refill?'refilled_at':'started_at'});return true;
};
