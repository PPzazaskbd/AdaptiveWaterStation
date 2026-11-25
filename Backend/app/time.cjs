function bad(message) {return Object.assign(new Error(message),{status:400});}
function bound(value, end) {
  if(value === null) return null;
  if (/^\d{4}-\d{2}-\d{2}$/.test(value)) {
    const parsed = Date.parse(value+'T00:00:00+07:00');
    if(!Number.isFinite(parsed) || new Date(parsed+7*3600000).toISOString().slice(0,10)!==value) throw bad('invalid_calendar_date');
    return new Date(parsed+(end?86400000:0)).toISOString();
  }
  if(!/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d{1,6})?(Z|[+-]\d{2}:\d{2})$/.test(value)||!Number.isFinite(Date.parse(value))) throw bad('use_YYYY_MM_DD_or_timestamp_with_timezone');
  return new Date(value).toISOString();
}
exports.dateRange = url => {
  const first=url.searchParams.get('from'),last=url.searchParams.get('to');
  if(first!==null&&last!==null&&(first.length===10)!==(last.length===10)) throw bad('do_not_mix_dates_and_timestamps');
  const from=bound(first,false),to=bound(last,true);
  if(from&&to&&Date.parse(from)>=Date.parse(to))throw bad('invalid_time_range');
  if(from&&to&&Date.parse(to)-Date.parse(from)>366*86400000)throw bad('maximum_range_is_366_days');
  return {from,to_exclusive:to,timezone:'Asia/Bangkok',date_only_to_is_inclusive:true};
};
exports.page = url => {
  const limit=url.searchParams.get('limit')??'20',offset=url.searchParams.get('offset')??'0';
  if(!/^\d{1,3}$/.test(limit)||+limit<1||+limit>100||!/^\d{1,5}$/.test(offset)||+offset>10000)throw bad('invalid_pagination');
  return {limit:+limit,offset:+offset};
};
