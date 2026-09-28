--竜宮の白タウナギ
function c37953640.initial_effect(c)
	--synchro custom
	local e1=Effect.CreateEffect(c)
	e1:SetType(EFFECT_TYPE_SINGLE)
	e1:SetCode(EFFECT_SYNCHRO_MATERIAL_CUSTOM)
	e1:SetProperty(EFFECT_FLAG_CANNOT_DISABLE+EFFECT_FLAG_UNCOPYABLE)
	e1:SetOperation(c37953640.synop)
	c:RegisterEffect(e1)
end
c37953640.tuner_filter=aux.FilterBoolFunction(Card.IsRace,RACE_FISH)
-- modern EFFECT_SYNCHRO_MATERIAL_CUSTOM: the synchro procedure calls this
-- with (e, tsg, ntsg, sg, lv, sc, tp) and expects (ok, lvchk), doing the level
-- math itself.  The old target/operation pair used an API this core no longer
-- calls that way, so the material check raised instead of returning a verdict.
function c37953640.synop(e,tsg,ntsg,sg,lv,sc,tp)
	local c=e:GetHandler()
	if sg:IsExists(function(tc) return not (tc:IsRace(RACE_FISH)) end,1,c) then
		return false
	end
	return true
end
